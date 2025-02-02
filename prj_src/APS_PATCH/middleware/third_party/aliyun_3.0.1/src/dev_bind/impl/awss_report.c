/*
 * Copyright (C) 2015-2018 Alibaba Group Holding Limited
 */
#include "dev_bind_internal.h"
#include "infra_config.h"

#ifndef GCC
#include "os.h"
#else
#include "os/os.h"
#endif
#if defined(__cplusplus)  /* If this is a C++ compiler, use C linkage */
extern "C" {
#endif

#define AWSS_REPORT_LEN_MAX       (256)
#define AWSS_TOKEN_TIMEOUT_MS     (120 * 1000)
#define MATCH_MONITOR_TIMEOUT_MS  (10 * 1000)
#define MATCH_REPORT_CNT_MAX      (2)

volatile char awss_report_token_suc = 0;
volatile char awss_report_token_cnt = 0;
static char awss_report_id = 0;
#ifdef WIFI_PROVISION_ENABLED
static uint8_t switchap_bssid[ETH_ALEN] = {0};
static char switchap_ssid[OS_MAX_SSID_LEN] = {0};
static char switchap_passwd[OS_MAX_PASSWD_LEN] = {0};
static void *switchap_timer = NULL;
#endif

static uint32_t awss_report_token_time = 0;
static void *report_token_timer = NULL;

//static int awss_report_token_to_cloud(void)
int awss_report_token_to_cloud(void);
#ifdef WIFI_PROVISION_ENABLED
static int awss_switch_ap_online(void);
static int awss_reboot_system(void);
#endif

#ifdef ALI_APP_TOKEN_BACKUP
uint8_t g_u8aAppToken[RANDOM_MAX_LEN] = {0};
#endif

int awss_token_remain_time()
{
    int remain = 0;
    uint32_t cur = os_get_time_ms();
    uint32_t diff = (uint32_t)(cur - awss_report_token_time);

    if (awss_report_token_suc == 0) {
        return remain;
    }

    if (diff < AWSS_TOKEN_TIMEOUT_MS) {
        remain = AWSS_TOKEN_TIMEOUT_MS - diff;
    }

    return remain;
}
void awss_report_token_to_cloud_timer(void)
{
	hal_ali_netlink_Task_MsgSend(ALI_NETLINK_AWSS_REPORT_TOKEN_TO_CLOUD, NULL, 0);
}

int awss_update_token()
{
    awss_report_token_time = 0;
    awss_report_token_cnt = 0;
    awss_report_token_suc = 0;

    if (report_token_timer == NULL) {
//				report_token_timer = HAL_Timer_Create("rp_token", (void (*)(void *))awss_report_token_to_cloud, NULL);
        report_token_timer = HAL_Timer_Create("rp_token", (void (*)(void *))awss_report_token_to_cloud_timer, NULL);
    }
    HAL_Timer_Stop(report_token_timer);
    HAL_Timer_Start(report_token_timer, 10);
    awss_info("update token");

    produce_random(aes_random, sizeof(aes_random));
    return 0;
}

int awss_token_timeout()
{
    if (awss_report_token_time == 0) {
        return 1;
    }

    uint32_t cur = os_get_time_ms();
    if ((uint32_t)(cur - awss_report_token_time) > AWSS_TOKEN_TIMEOUT_MS) {
        return 1;
    }
    return 0;
}

void awss_report_token_reply(void *pcontext, void *pclient, void *msg)
{
    int ret, len;
    char *payload;
    char *id = NULL;
    char reply_id = 0;
    uint32_t payload_len;

    ret = awss_cmp_mqtt_get_payload(msg, &payload, &payload_len);
    
//    printf("xxxawss_report_token_reply-->%d\r\n", ret);

    if (ret != 0 || payload == NULL || payload_len == 0) {
        return;
    }

    id = json_get_value_by_name(payload, payload_len, AWSS_JSON_ID, &len, NULL);
    if (id == NULL) {
        return;
    }

    reply_id = atoi(id);
    if (reply_id + 1 < awss_report_id) {
        return;
    }
    awss_info("%s\r\n", __func__);
    awss_report_token_suc = 1;
//    printf("\r\nbbbbbbbawss_report_token_suc=%d\r\n", awss_report_token_suc);
    iotx_event_post(IOTX_CONN_REPORT_TOKEN_SUC);
    awss_stop_timer(report_token_timer);
    report_token_timer = NULL;
    AWSS_DB_UPDATE_STATIS(AWSS_DB_STATIS_SUC);
    AWSS_DB_DISP_STATIS();
    return;
}

#ifdef WIFI_PROVISION_ENABLED
void awss_online_switchap(void *pcontext, void *pclient, void *msg)
{
#define SWITCHAP_RSP_LEN   (64)
#define AWSS_BSSID_STR_LEN (17)
#define AWSS_SSID          "ssid"
#define AWSS_PASSWD        "passwd"
#define AWSS_BSSID         "bssid"
#define AWSS_SWITCH_MODE   "switchMode"
#define AWSS_TIMEOUT       "timeout"

    int len = 0, timeout = 0;
    char *packet = NULL, *awss_info = NULL, *elem = NULL;
    int packet_len = SWITCHAP_RSP_LEN, awss_info_len = 0;

    uint32_t payload_len;
    char *payload;
    int ret;

    ret = awss_cmp_mqtt_get_payload(msg, &payload, &payload_len);

    if (ret != 0) {
        goto ONLINE_SWITCHAP_FAIL;
    }

    if (payload == NULL || payload_len == 0) {
        goto ONLINE_SWITCHAP_FAIL;
    }

    awss_debug("online switchap len:%u, payload:%s\r\n", payload_len, payload);
    packet = os_zalloc_(packet_len + 1);
    if (packet == NULL) {
        goto ONLINE_SWITCHAP_FAIL;
    }

    awss_info = json_get_value_by_name(payload, payload_len, AWSS_JSON_PARAM, &awss_info_len, NULL);
    if (awss_info == NULL || awss_info_len == 0) {
        goto ONLINE_SWITCHAP_FAIL;
    }

    /*
     * get SSID , PASSWD, BSSID of router
     */
    elem = json_get_value_by_name(awss_info, awss_info_len, AWSS_SSID, &len, NULL);
    if (elem == NULL || len <= 0 || len >= OS_MAX_SSID_LEN) {
        goto ONLINE_SWITCHAP_FAIL;
    }

    memset(switchap_ssid, 0, sizeof(switchap_ssid));
    memcpy(switchap_ssid, elem, len);

    len = 0;
    elem = json_get_value_by_name(awss_info, awss_info_len, AWSS_PASSWD, &len, NULL);
    if (elem == NULL || len <= 0 || len >= OS_MAX_PASSWD_LEN) {
        goto ONLINE_SWITCHAP_FAIL;
    }

    memset(switchap_passwd, 0, sizeof(switchap_passwd));
    memcpy(switchap_passwd, elem, len);

    len = 0;
    memset(switchap_bssid, 0, sizeof(switchap_bssid));
    elem = json_get_value_by_name(awss_info, awss_info_len, AWSS_BSSID, &len, NULL);

    if (elem != NULL && len == AWSS_BSSID_STR_LEN) {
        uint8_t i = 0;
        char *bssid_str = elem;
        // convert bssid string to bssid value
        while (i < OS_ETH_ALEN) {
            switchap_bssid[i ++] = (uint8_t)strtol(bssid_str, &bssid_str, 16);
            ++ bssid_str;
            /*
             * fix the format of bssid string is not legal.
             */
            if ((uint32_t)((unsigned long)bssid_str - (unsigned long)elem) > AWSS_BSSID_STR_LEN) {
                memset(switchap_bssid, 0, sizeof(switchap_bssid));
                break;
            }
        }
    }

    len = 0;
    elem = json_get_value_by_name(awss_info, awss_info_len, AWSS_SWITCH_MODE, &len, NULL);
    if (elem != NULL && (elem[0] == '0' || elem[0] == 0)) {
        elem = json_get_value_by_name(awss_info, awss_info_len, AWSS_TIMEOUT, &len, NULL);
        if (elem) {
            timeout = (int)strtol(elem, &elem, 16);
        }
    }

    do {
        // reduce stack used
        char *id = NULL;
        char id_str[MSG_REQ_ID_LEN] = {0};
        id = json_get_value_by_name(payload, payload_len, AWSS_JSON_ID, &len, NULL);
        memcpy(id_str, id, len > MSG_REQ_ID_LEN - 1 ? MSG_REQ_ID_LEN - 1 : len);
        awss_build_packet(AWSS_CMP_PKT_TYPE_RSP, id_str, ILOP_VER, METHOD_EVENT_ZC_SWITCHAP, "{}", 200, packet, &packet_len);
    } while (0);

    do {
        char reply[TOPIC_LEN_MAX] = {0};
        awss_build_topic(TOPIC_SWITCHAP_REPLY, reply, TOPIC_LEN_MAX);
        awss_cmp_mqtt_send(reply, packet, packet_len, 1);
        HAL_Free(packet);
    } while (0);

    /*
     * make sure the response would been received
     */
    if (timeout < 1000) {
        timeout = 1000;
    }

    do {
        uint8_t bssid[ETH_ALEN] = {0};
        char ssid[OS_MAX_SSID_LEN + 1] = {0}, passwd[OS_MAX_PASSWD_LEN + 1] = {0};
        HAL_Wifi_Get_Ap_Info(ssid, passwd, bssid);
        /*
         * switch ap when destination ap is differenct from current ap
         */
        if (strncmp(ssid, switchap_ssid, sizeof(ssid)) ||
            memcmp(bssid, switchap_bssid, sizeof(bssid)) ||
            strncmp(passwd, switchap_passwd, sizeof(passwd))) {
            if (switchap_timer == NULL) {
                switchap_timer = HAL_Timer_Create("swichap_online", (void (*)(void *))awss_switch_ap_online, NULL);
            }

            HAL_Timer_Stop(switchap_timer);
            HAL_Timer_Start(switchap_timer, timeout);
        }
    } while (0);

    return;

ONLINE_SWITCHAP_FAIL:
    awss_warn("ilop online switchap failed");
    memset(switchap_ssid, 0, sizeof(switchap_ssid));
    memset(switchap_bssid, 0, sizeof(switchap_bssid));
    memset(switchap_passwd, 0, sizeof(switchap_passwd));
    if (packet) {
        HAL_Free(packet);
    }
    return;
}

static void *reboot_timer = NULL;
static int awss_switch_ap_online()
{
    HAL_Awss_Connect_Ap(WLAN_CONNECTION_TIMEOUT_MS, switchap_ssid, switchap_passwd,
                        AWSS_AUTH_TYPE_INVALID, AWSS_ENC_TYPE_INVALID, switchap_bssid, 0);

    awss_stop_timer(switchap_timer);
    switchap_timer = NULL;

    memset(switchap_ssid, 0, sizeof(switchap_ssid));
    memset(switchap_bssid, 0, sizeof(switchap_bssid));
    memset(switchap_passwd, 0, sizeof(switchap_passwd));

    reboot_timer = HAL_Timer_Create("rb_timer", (void (*)(void *))awss_reboot_system, NULL);

    if(reboot_timer)
    {
        HAL_Timer_Stop(reboot_timer);
    HAL_Timer_Start(reboot_timer, 1000);;
    }

    return 0;
}

static int awss_reboot_system()
{
    awss_stop_timer(reboot_timer);
    reboot_timer = NULL;
    HAL_Reboot();
    while (1);
//    return 0;
}
#endif
//static int awss_report_token_to_cloud()
int awss_report_token_to_cloud()
{
#define REPORT_TOKEN_PARAM_LEN  (64)
    if (awss_report_token_suc) { // success ,no need to report
        return 0;
    }

    AWSS_DB_UPDATE_STATIS(AWSS_DB_STATIS_START);

    /*
     * it is still failed after try to report token MATCH_REPORT_CNT_MAX times
     */
    if (awss_report_token_cnt ++ > MATCH_REPORT_CNT_MAX) {
        awss_stop_timer(report_token_timer);
        report_token_timer = NULL;
        awss_info("try %d times fail", awss_report_token_cnt);
        return -2;
    }

    if (report_token_timer == NULL) {
//				report_token_timer = HAL_Timer_Create("rp_token", (void (*)(void *))awss_report_token_to_cloud, NULL);
        report_token_timer = HAL_Timer_Create("rp_token", (void (*)(void *))awss_report_token_to_cloud_timer, NULL);
    }
    HAL_Timer_Stop(report_token_timer);
    HAL_Timer_Start(report_token_timer, 3 * 1000);

    int packet_len = AWSS_REPORT_LEN_MAX;

    char *packet = os_zalloc_(packet_len + 1);
    if (packet == NULL) {
        awss_err("alloc mem(%d) failed", packet_len);
        return -1;
    }

    do {
        // reduce stack used
        uint8_t i;
        char id_str[MSG_REQ_ID_LEN] = {0};
        char param[REPORT_TOKEN_PARAM_LEN] = {0};
        char token_str[(RANDOM_MAX_LEN << 1) + 1] = {0};

        #ifdef ALI_APP_TOKEN_BACKUP
        for (i = 0; i < sizeof(g_u8aAppToken); i ++)  /* check g_u8aAppToken is initialed or not */
            if (g_u8aAppToken[i] != 0x00) {
                break;
            }

        if (i >= sizeof(g_u8aAppToken)) { /* g_u8aAppToken needs to be initialed */
            produce_random(g_u8aAppToken, sizeof(g_u8aAppToken));
        }
        #else
        for (i = 0; i < sizeof(aes_random); i ++)  /* check aes_random is initialed or not */
            if (aes_random[i] != 0x00) {
                break;
            }

        if (i >= sizeof(aes_random)) { /* aes_random needs to be initialed */
            produce_random(aes_random, sizeof(aes_random));
        }
        #endif

        awss_report_token_time = os_get_time_ms();
        
//        printf("awss_report_token_to_cloud -----aes_random(%d):\r\n", RANDOM_MAX_LEN);
//        for(int i=0;i<RANDOM_MAX_LEN;i++){						
//             printf("0x%02x ", aes_random[i]);				
//        }
//        printf("\r\n");
//        printf("\r\n");
        HAL_Snprintf(id_str, MSG_REQ_ID_LEN - 1, "\"%u\"", awss_report_id ++);

        #ifdef ALI_APP_TOKEN_BACKUP
        utils_hex_to_str(g_u8aAppToken, RANDOM_MAX_LEN, token_str, sizeof(token_str) - 1);
        #else
        utils_hex_to_str(aes_random, RANDOM_MAX_LEN, token_str, sizeof(token_str) - 1);
        #endif

        HAL_Snprintf(param, REPORT_TOKEN_PARAM_LEN - 1, "{\"token\":\"%s\"}", token_str);
        awss_build_packet(AWSS_CMP_PKT_TYPE_REQ, id_str, ILOP_VER, METHOD_MATCH_REPORT, param, 0, packet, &packet_len);
    } while (0);

    awss_debug("report token:%s\r\n", packet);
    char topic[TOPIC_LEN_MAX] = {0};
    awss_build_topic(TOPIC_MATCH_REPORT, topic, TOPIC_LEN_MAX);

    int ret = awss_cmp_mqtt_send(topic, packet, packet_len, 1);
    awss_info("report token res:%d\r\n", ret);
    HAL_Free(packet);

    return ret;
}

int awss_report_token()
{
    awss_report_token_cnt = 0;
    awss_report_token_suc = 0;

    return awss_report_token_to_cloud();
}

int awss_stop_report_token()
{
    if (report_token_timer) {
        awss_stop_timer(report_token_timer);
        report_token_timer = NULL;
    }

    memset(aes_random, 0x00, sizeof(aes_random));

    return 0;
}

#if defined(__cplusplus)  /* If this is a C++ compiler, use C linkage */
}
#endif
