/*
 * Copyright (C) 2015-2018 Alibaba Group Holding Limited
 */
#include "infra_config.h"

#ifdef INFRA_PREAUTH

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "infra_types.h"
#include "infra_defs.h"
#include "infra_httpc.h"
#include "infra_preauth.h"
#include "infra_string.h"

#define PREAUTH_HTTP_REQ_LEN            300
#define PREAUTH_HTTP_RSP_LEN            300

#define PREAUTH_IOT_ID_MAXLEN           (64)
#define PREAUTH_IOT_TOKEN_MAXLEN        (65)
#define PREAUTH_IOT_HOST_MAXLEN         (64)

#ifndef CONFIG_GUIDER_AUTH_TIMEOUT
    #define CONFIG_GUIDER_AUTH_TIMEOUT      (10 * 1000)
#endif

#ifdef SUPPORT_TLS
    extern const char *iotx_ca_crt;
#endif

SHM_DATA int _preauth_assemble_auth_req_string(const iotx_dev_meta_info_t *dev_meta, const char *sign,
                                      const char *device_id, char *request_buff, uint32_t buff_len)
{
    uint8_t i = 0;
    const char *kv[][2] = {
        { "productKey", NULL },
        { "deviceName", NULL },
        { "signmethod", "hmacsha256"},
        { "sign", NULL },
        { "version", "default" },
        { "clientId", NULL },
        { "timestamp", "2524608000000" },
        { "resources", "mqtt" }
    };

    if (dev_meta == NULL || sign == NULL || device_id == NULL || request_buff == NULL) {
        return FAIL_RETURN;
    }

    kv[0][1] = dev_meta->product_key;
    kv[1][1] = dev_meta->device_name;
    kv[3][1] = sign;
    kv[5][1] = device_id;

    for (i = 0; i < (sizeof(kv) / (sizeof(kv[0]))); i++) {
        if ((strlen(request_buff) + strlen(kv[i][0]) + strlen(kv[i][1]) + 2) >=
            buff_len) {
            return FAIL_RETURN;
        }

        memcpy(request_buff + strlen(request_buff), kv[i][0], strlen(kv[i][0]));
        memcpy(request_buff + strlen(request_buff), "=", 1);
        memcpy(request_buff + strlen(request_buff), kv[i][1], strlen(kv[i][1]));
        memcpy(request_buff + strlen(request_buff), "&", 1);
    }

    memset(request_buff + strlen(request_buff) - 1, '\0', 1);
    return SUCCESS_RETURN;
}

SHM_DATA static int _preauth_get_string_value(char *p_string, char *value_buff, uint32_t buff_len)
{
    char *p = p_string;
    char *p_start = NULL;
    char *p_end = NULL;
    uint32_t len = 0;

    while (*(++p) != ',' || *p != '}') {
        if (*p == '\"') {
            if (p_start) {
                p_end = p;
                break;
            } else {
                p_start = p + 1;
            }
        }
    }

    if (p_start == NULL || p_end == NULL) {
        return FAIL_RETURN;
    }

    len = p_end - p_start;
    if (len > buff_len) {
        return FAIL_RETURN;
    }

    memcpy(value_buff, p_start, len);
    return SUCCESS_RETURN;
}

SHM_DATA static int _preauth_parse_auth_rsp_string(char *json_string, uint32_t string_len, iotx_sign_mqtt_t *output)
{
    int res = FAIL_RETURN;
    char *p = json_string;
    char *p_start, *p_end, *pt;
    uint8_t len;
    int code = 0;

    while (p < (json_string + string_len)) {
        while (*(++p) != ':') {
            if (p >= (json_string + string_len)) {
                if (code != 200) {
                    return FAIL_RETURN;
                }
                else {
                    return SUCCESS_RETURN;
                }
            }
        }

        pt = p;
        p_start = p_end = NULL;
        while (--pt > json_string) {
            if (*pt == '\"') {
                if (p_end != NULL) {
                    p_start = pt + 1;
                    break;
                } else {
                    p_end = pt;
                }
            }
        }

        if (p_start == NULL || p_end == NULL) {
            return FAIL_RETURN;
        }
        len = p_end - p_start;

        if (strlen("code") == len && !memcmp(p_start, "code", len)) {
            infra_str2int(++p, &code);
            if (code != 200) {
                return FAIL_RETURN;
            }
        } else if (strlen("iotId") == len && !memcmp(p_start, "iotId", len)) {
            res = _preauth_get_string_value(p, output->username, PREAUTH_IOT_ID_MAXLEN);
            if (res < SUCCESS_RETURN) {
                return res;
            }
        } else if (strlen("iotToken") == len && !memcmp(p_start, "iotToken", len)) {
            res = _preauth_get_string_value(p, output->password, PREAUTH_IOT_TOKEN_MAXLEN);
            if (res < SUCCESS_RETURN) {
                return res;
            }
        } else if (strlen("host") == len && !memcmp(p_start, "host", len)) {
            res = _preauth_get_string_value(p, output->hostname, PREAUTH_IOT_HOST_MAXLEN);
            if (res < SUCCESS_RETURN) {
                return res;
            }
        } else if (strlen("port") == len && !memcmp(p_start, "port", len)) {
            int port_temp;
            infra_str2int(++p, &port_temp);
            output->port = port_temp;
        }
    }

    return SUCCESS_RETURN;
}

int _fill_conn_string(char *dst, int len, const char *fmt, ...)
{
//    int                     rc = -1;
    va_list                 ap;
    char                   *ptr = NULL;

    va_start(ap, fmt);
//    rc = HAL_Vsnprintf(dst, len, fmt, ap);
    HAL_Vsnprintf(dst, len, fmt, ap);
    va_end(ap);
//    LITE_ASSERT(rc <= len);

    ptr = strstr(dst, "||");
    if (ptr) {
        *ptr = '\0';
    }

    /* sys_debug("dst(%d) = %s", rc, dst); */
    return 0;
}

SHM_DATA int preauth_get_connection_info(iotx_mqtt_region_types_t region, iotx_dev_meta_info_t *dev_meta,
                                const char *sign, const char *device_id, iotx_sign_mqtt_t *preauth_output)
{
    char http_url[128] = "http://";
    char http_url_frag[] = "/auth/devicename";
#ifdef SUPPORT_TLS
    int http_port = 443;
#else
    int http_port = 80;
#endif
    int res = FAIL_RETURN;
    httpclient_t httpc;
    httpclient_data_t httpc_data;
    char request_buff[PREAUTH_HTTP_REQ_LEN] = {0};
    char response_buff[PREAUTH_HTTP_RSP_LEN] = {0};

    if (g_infra_http_domain[region] == NULL) {
        return FAIL_RETURN;
    }

    memset(&httpc, 0, sizeof(httpclient_t));
    memset(&httpc_data, 0, sizeof(httpclient_data_t));
    memcpy(http_url + strlen(http_url), g_infra_http_domain[region], strlen(g_infra_http_domain[region]));
    memcpy(http_url + strlen(http_url), http_url_frag, sizeof(http_url_frag));

    httpc.header = "Accept: text/xml,text/javascript,text/html,application/json\r\n";

    _preauth_assemble_auth_req_string(dev_meta, sign, device_id, request_buff, sizeof(request_buff));

    httpc_data.post_content_type = "application/x-www-form-urlencoded;charset=utf-8";
    httpc_data.post_buf = request_buff;
    httpc_data.post_buf_len = strlen(request_buff);
    httpc_data.response_buf = response_buff;
    httpc_data.response_buf_len = sizeof(response_buff);

#ifdef ALI_HTTP_COMPATIBLE
    char *sCaCrt = NULL;

    if(g_u8UseHttp)
    {
        http_port = 80;
    }
    else
    {
        http_port = 443;
        sCaCrt = iotx_ca_crt;
    }

    res = httpclient_common_(&httpc,
                            http_url,
                            http_port,
                            sCaCrt,
                            HTTPCLIENT_POST,
                            CONFIG_GUIDER_AUTH_TIMEOUT,
                            &httpc_data);
#else //#ifdef ALI_HTTP_COMPATIBLE
    res = httpclient_common_(&httpc,
                            http_url,
                            http_port,
#ifdef SUPPORT_TLS
                            iotx_ca_crt,
#else
                            NULL,
#endif
                            HTTPCLIENT_POST,
                            CONFIG_GUIDER_AUTH_TIMEOUT,
                            &httpc_data);
#endif //#ifdef ALI_HTTP_COMPATIBLE

    if (res < SUCCESS_RETURN) {
        return res;
    }

#ifdef INFRA_LOG_NETWORK_PAYLOAD
    preauth_info("Downstream Payload:");
    iotx_facility_json_print(response_buff, LOG_INFO_LEVEL, '<');
#endif
    
//    const char     *p_domain = NULL;
    char            iotx_conn_host[HOST_ADDRESS_LEN + 1] = {0};
    uint16_t        iotx_conn_port = 1883;
    char            iotx_id[GUIDER_IOT_ID_LEN + 1] = {0};
    char            iotx_token[GUIDER_IOT_TOKEN_LEN + 1] = {0};
    int len=0;
    
    /* http auth */
    if(iotx_redirect_get_flag()){
        if(iotx_redirect_get_iotId_iotToken(iotx_id,iotx_token,iotx_conn_host,&iotx_conn_port) != 0){
            iotx_redirect_set_flag(0);
            goto failed;
        }
        
         /* setup the hostname and port */
        preauth_output->port = iotx_conn_port;
        len = strlen(iotx_conn_host) + 1;
//        conn->host_name = SYS_GUIDER_MALLOC(len);
//        if (conn->host_name == NULL) {
//            goto failed;
//        }
        _fill_conn_string(preauth_output->hostname, len,
                          "%s",
                          iotx_conn_host);

        /* fill up username and password */
        len = strlen(iotx_id) + 1;
//        preauth_output->username = SYS_GUIDER_MALLOC(len);
//        if (conn->username == NULL) {
//            goto failed;
//        }
        _fill_conn_string(preauth_output->username, len, "%s", iotx_id);
        len = strlen(iotx_token) + 1;
//        preauth_output->password = SYS_GUIDER_MALLOC(len);
//        if (conn->password == NULL) {
//            goto failed;
//        }
        _fill_conn_string(preauth_output->password, len, "%s", iotx_token);
    }else{
    
        res = _preauth_parse_auth_rsp_string(response_buff, strlen(response_buff), preauth_output);
        

    }
 
    
    
    return res;
failed:
    memset(&preauth_output, 0, sizeof(preauth_output));
    return -1;    
    
    
}

#endif /* #ifdef MQTT_PRE_AUTH */
