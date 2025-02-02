/******************************************************************************
*  Copyright 2019, Netlink Communication Corp.
*  ---------------------------------------------------------------------------
*  Statement:
*  ----------
*  This software is protected by Copyright and the information contained
*  herein is confidential. The software may not be copied and the information
*  contained herein may not be used or disclosed except with the written
*  permission of Netlink Communication Corp. (C) 2019
******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "cmsis_os.h"
#include "at_cmd.h"
#include "at_cmd_common.h"
#include "at_cmd_tcpip.h"
#include "at_cmd_nvm.h"
#include "at_cmd_data_process.h"
#include "hal_wdt.h"
#include "sys_os_config.h"
#include "lwip/netdb.h"
#include "ping_cmd.h"
#include "controller_wifi.h"

#include "at_cmd_tcpip_patch.h"
#include "at_cmd_common_patch.h"

extern int at_trans_client_init_impl(at_nvm_trans_config_t *trans_cfg);
extern int at_cmd_tcpip_savetranslink(char *buf, int len, int mode);
extern int at_cmd_tcpip_cipserver(char *buf, int len, int mode);
extern int at_cmd_tcpip_cipstart(char *buf, int len, int mode);
extern void at_create_tcpip_tx_task_impl(void);


extern ping_result_t g_tPingResult;
extern volatile uint8_t g_u8PingCmdDone;
extern volatile bool ipd_info_enable;
/******************************************************
 *                      Macros
 ******************************************************/
//#define AT_CMD_TCPIP_DBG

#ifdef AT_CMD_TCPIP_DBG
    #define AT_LOG                      printf(("[AT]: "fmt"\r\n"), ##arg)
#else
    #define AT_LOG(...)
#endif

#define AT_LOGI(fmt,arg...)             printf(("[AT]: "fmt"\r\n"), ##arg)

#define IP_STR_BUF_LEN                  16
#define AT_CMD_PING_SERV_PORT_STR       "20" // not used
#define AT_CMD_PING_RECV_TIMEOUT        1000 // ms
#define AT_CMD_PING_PERIOD              10 // ms
#define AT_CMD_PING_NUM                 4
#define AT_CMD_PING_SIZE                32
#define AT_CMD_PING_WAIT_RSP            10 // ms
#define AT_CMD_PING_WAIT_RSP_MAX_CNT    (((AT_CMD_PING_RECV_TIMEOUT + AT_CMD_PING_PERIOD) * (AT_CMD_PING_NUM + 1)) / AT_CMD_PING_WAIT_RSP)

/******************************************************
 *               Variable Definitions
 ******************************************************/
extern osThreadId   at_tx_task_id;
extern osPoolId     at_tx_task_pool_id;
extern osMessageQId at_tx_task_queue_id;
extern at_command_t *g_AtCmdTbl_Tcpip_Ptr;
extern uint32_t at_tx_data_task_stack_size;
extern volatile bool at_ipMux;
extern volatile bool at_ip_mode; // 0: normal transmission mode. 1:transparent transmission
extern int at_tx_data_task_priority;
extern const osPoolDef_t os_pool_def_at_tx_task_pool;
extern int at_netconn_max_num;
extern volatile uint16_t server_timeover;
extern uint8_t g_server_mode;
extern uint32_t g_server_port;
extern int tcp_server_socket;
extern at_socket_t atcmd_socket[AT_LINK_MAX_NUM];
extern uint8_t sending_id;
extern uint32_t at_send_len;
extern uint8_t *pDataLine;
extern uint8_t  at_data_line[];

/******************************************************
 *               Function Definitions
 ******************************************************/
static void at_server_timeout_handler_patch(void)
{
    uint8_t loop = 0;
    at_socket_t *link = NULL;
    //char resp_buf[32];
    
    for (loop = 0; loop < at_netconn_max_num; loop++) {
        link = at_link_get_id(loop);
        if ((link->sock >= 0) && (link->terminal_type == AT_REMOTE_CLIENT)) {
            link->server_timeout++;

            if (link->server_timeout >= server_timeover) {
                at_socket_client_cleanup_task(link);
                //at_sprintf(resp_buf, "%d,CLOSED\r\n", loop);
                //msg_print_uart1(resp_buf);
                msg_print_uart1("%d,CLOSED\r\n", loop);
            }
        }
    }
    AT_LOGI("at_server_timeout_handler\r\n");
}

static void at_socket_process_task_patch(void *arg)
{
    at_socket_t* plink = (at_socket_t*)arg;
    bool link_count_flag = false;

    if (plink == NULL) {
        plink->task_handle = NULL;
        vTaskDelete(NULL);
    }

    //xSemaphoreTake(plink->sema,0);
    if ((plink->link_type == AT_LINK_TYPE_TCP) || (plink->link_type == AT_LINK_TYPE_SSL)) {
        link_count_flag = true;
        at_update_link_count(1);
    }

    for(;;)
    {
        if ((plink->link_state == AT_LINK_DISCONNECTED) || (plink->link_state == AT_LINK_DISCONNECTING)) {
            if (at_ip_mode == true) {
                if (at_create_tcp_client_trans(plink) < 0) {
                    vTaskDelay(g_at_trans_recreate_ms);
                    continue;
                }
            }
            else {
                break;
            }
        }

        at_process_recv_socket(plink);
        Hal_Wdt_Clear();
    }

    if (link_count_flag) {
        at_update_link_count(-1);
    }

    if (plink->recv_buf) {
        free(plink->recv_buf);
        plink->recv_buf = NULL;
        AT_LOGI("plink->recv_buf=%p\r\n", plink->recv_buf);
    }

    AT_LOGI("socket recv delete\r\n");

    if(plink->link_state == AT_LINK_DISCONNECTING) {
        //xSemaphoreGive(plink->sema);
        plink->link_state = AT_LINK_DISCONNECTED;
    }

    plink->task_handle = NULL;
    vTaskDelete(NULL);
}


int at_trans_client_init_patch(at_nvm_trans_config_t *trans_cfg)
{
    if (at_trans_client_init_impl(trans_cfg) == AT_RESULT_CODE_OK) {
        at_create_tcpip_data_task();
        return AT_RESULT_CODE_OK;
    }
    else {
        return AT_RESULT_CODE_ERROR;
    }
}
 
void at_create_tcpip_tx_task_patch(void)
{
    if (at_tx_task_id != NULL)
        return;
    at_create_tcpip_tx_task_impl();
}

/*
 * @brief Command at+cipstart
 *
 * @param [in] argc count of parameters
 *
 * @param [in] argv parameters array
 *
 * @return 0 fail 1 success
 *
 */
int at_cmd_tcpip_cipstart_patch(char *buf, int len, int mode)
{
    char *argv[AT_MAX_CMD_ARGS] = {0};
    int argc = 0;
    uint8_t linkID;
    at_socket_t *link;
    char *tmp_buf = NULL;
    
    tmp_buf = malloc(len);
    if (tmp_buf == NULL) {
        goto exit;
    }
    memcpy(tmp_buf, buf, len);
    tmp_buf[len] = '\0';
    
    at_cmd_tcpip_cipstart(buf, len, mode);
    
    if (!at_cmd_buf_to_argc_argv(tmp_buf, &argc, argv, AT_MAX_CMD_ARGS))
        goto exit;

    if (at_ipMux)
        linkID = atoi(argv[1]);
    else
        linkID = 0;
    
    link = at_link_get_id(linkID);
    if (link->sock >= 0) {
        at_create_tcpip_data_task();
    }

exit:
    if (tmp_buf) free(tmp_buf);
    return true;
}

/*
 * @brief Command at+cipserver
 *
 * @param [in] argc count of parameters
 *
 * @param [in] argv parameters array
 *
 * @return 0 fail 1 success
 *
 */
int at_cmd_tcpip_cipserver_patch(char *buf, int len, int mode)
{
    at_cmd_tcpip_cipserver(buf, len, mode);
    if (tcp_server_socket >= 0) {
        at_create_tcpip_data_task();
    }
    return true;
}

/*
 * @brief Command at+cipsend
 *
 * @param [in] argc count of parameters
 *
 * @param [in] argv parameters array
 *
 * @return 0 fail 1 success
 *
 */
int at_cmd_tcpip_cipsend_patch(char *buf, int len, int mode)
{
    char *param = NULL;
    at_socket_t *link;
    int send_id = 0;
    int send_len = 0;
    uint8_t ret = AT_RESULT_CODE_ERROR;
    char *pSavedPtr = NULL;

    switch (mode) {
        case AT_CMD_MODE_EXECUTION:
            if (at_ipMux == true || at_ip_mode == false) {
                goto exit;
            }

            if (atcmd_socket[0].sock < 0) {
                goto exit;
            }

            atcmd_socket[0].link_state = AT_LINK_TRANSMIT_SEND;

            sending_id = send_id;
            at_send_len = 0;
            pDataLine = at_data_line;

            at_cmd_trans_lock();

            at_response_result(AT_RESULT_CODE_OK);
            msg_print_uart1("\r\n> ");
            ret = AT_RESULT_CODE_IGNORE;
            break;

        case AT_CMD_MODE_SET:  // AT+CIPSEND= link,<op>
            param = strtok_r(buf, "=", &pSavedPtr);

            if (at_ip_mode == true) {
                goto exit;
            } 
            
            if (at_ipMux) {
                /* Multiple connections */
                param = strtok_r(NULL, ",", &pSavedPtr);
                
                if (at_cmd_get_para_as_digital(param, &send_id) != 0) {
                    goto exit;
                }

                if (send_id >= AT_LINK_MAX_NUM) {
                    goto exit;
                }
            } else {
                send_id = 0;
            }

            link = at_link_get_id(send_id);
            if (link->sock < 0) {
                AT_LOG("link is not connected\r\n");
                goto exit;
            }

            param = strtok_r(NULL, "\0", &pSavedPtr);

            if (at_cmd_get_para_as_digital(param, &send_len) != 0) {
                goto exit;
            }
                
            if (send_len > AT_DATA_LEN_MAX) {
               AT_LOG("data length is too long\r\n");
               goto exit;
            }


            if (link->link_type == AT_LINK_TYPE_UDP)
            {
              //TODO: Remote IP and ports can be set in UDP transmission:
              //AT+CIPSEND=[<link ID>,]<length>[,<remote IP>,<remote port>]
            }

            //switch port input to TCP/IP module
            sending_id = send_id;
            at_send_len = send_len;
            pDataLine = at_data_line;
            data_process_lock(LOCK_TCPIP, at_send_len);
            at_response_result(AT_RESULT_CODE_OK);
            msg_print_uart1("\r\n> ");
            ret = AT_RESULT_CODE_IGNORE;
            break;

        default :
            break;
    }

exit:
    at_response_result(ret);
    return ret;
}

/*
 * @brief Command at+savetranslink
 *
 * @param [in] argc count of parameters
 *
 * @param [in] argv parameters array
 *
 * @return 0 fail 1 success
 *
 */
int at_cmd_tcpip_savetranslink_patch(char *buf, int len, int mode)
{
    int enable = 0;
    int argc = 0;
    char *argv[AT_MAX_CMD_ARGS] = {0};
    char *tmp_buf = NULL;
    
    if (mode == AT_CMD_MODE_SET) {
        tmp_buf = malloc(len + 1);
        if (tmp_buf == NULL) {
            goto exit;
        }
        memcpy(tmp_buf, buf, len);
        tmp_buf[len] = '\0';
        
        if (!at_cmd_buf_to_argc_argv(tmp_buf, &argc, argv, AT_MAX_CMD_ARGS)) {
            goto exit;
        }
        
        if (at_cmd_get_para_as_digital(argv[1], &enable) != 0) {
            at_response_result(AT_RESULT_CODE_ERROR);
            goto exit;
        }
        
        if (enable > 1 || enable < 0) {
            at_response_result(AT_RESULT_CODE_ERROR);
            goto exit;
        }
        
        at_cmd_tcpip_savetranslink(buf, len, mode);
    }
    
exit:
    if (tmp_buf) free(tmp_buf);
    return true;
}

/*
 * @brief Command at+ping
 *
 * @param [in] argc count of parameters
 *
 * @param [in] argv parameters array
 *
 * @return 0 fail 1 success
 *
 */
int at_cmd_tcpip_ping_patch(char *buf, int len, int mode)
{
    int iRet = 0;
    char *argv[AT_MAX_CMD_ARGS] = {0};
    int argc = 0;
    char baIpStrBuf[IP_STR_BUF_LEN] = {0};
    char *sTarget = NULL;
    uint32_t u32Cnt = 0;

    if(mode != AT_CMD_MODE_SET)
    {
        AT_LOG("[%s %d] invalid mode[%d]\n", __func__, __LINE__, mode);
        goto done;
    }

    if(!at_cmd_is_tcpip_ready())
    {
        AT_LOG("[%s %d] at_cmd_is_tcpip_ready fail\n", __func__, __LINE__);
        goto done;
    }

    if(!at_cmd_buf_to_argc_argv(buf, &argc, argv, AT_MAX_CMD_ARGS))
    {
        AT_LOG("[%s %d] at_cmd_buf_to_argc_argv fail\n", __func__, __LINE__);
        goto done;
    }

    if(argc < 2)
    {
        AT_LOG("[%s %d] no param\n", __func__, __LINE__);
        goto done;
    }

    AT_LOG("[%s %d] argc[%d] argv[1]: [%s][%d]\n", __func__, __LINE__, argc, argv[1], strlen(argv[1]));

    //sTarget = at_cmd_param_trim(argv[1]);
    sTarget = argv[1];

    if(!sTarget)
    {
        AT_LOG("[%s %d] invalid param\n", __func__, __LINE__);
        goto done;
    }

    if(at_cmd_is_valid_ip(sTarget))
    {
        strcpy(baIpStrBuf, sTarget);
    }
    else
    {
        struct addrinfo *ptAddrInfo = NULL;

        if(getaddrinfo(sTarget, AT_CMD_PING_SERV_PORT_STR, NULL, &ptAddrInfo))
        {
            AT_LOG("[%s %d] getaddrinfo fail\n", __func__, __LINE__);
            goto done;
        }

        if(ptAddrInfo)
        {
            struct sockaddr_in *ptAddrIn = (struct sockaddr_in *)ptAddrInfo->ai_addr;

            snprintf(baIpStrBuf, sizeof(baIpStrBuf), "%s", inet_ntoa(ptAddrIn->sin_addr));
            freeaddrinfo(ptAddrInfo);
        }
        else
        {
            AT_LOG("[%s %d] ptAddrInfo is NULL\n", __func__, __LINE__);
            goto done;
        }
    }

    AT_LOG("[%s %d] baIpStrBuf[%s][%d]\n", __func__, __LINE__, baIpStrBuf, strlen(baIpStrBuf));

    g_tPingResult.recv_num = 0;

    g_u8PingCmdDone = 0;

    ping_request(AT_CMD_PING_NUM, baIpStrBuf, PING_IP_ADDR_V4, AT_CMD_PING_SIZE, AT_CMD_PING_RECV_TIMEOUT, AT_CMD_PING_PERIOD, at_cmd_ping_callback);

    while(u32Cnt < AT_CMD_PING_WAIT_RSP_MAX_CNT)
    {
        if(g_u8PingCmdDone)
        {
            if(g_tPingResult.recv_num)
            {
                iRet = 1;
            }

            break;
        }

        osDelay(AT_CMD_PING_WAIT_RSP);
        ++u32Cnt;
    }

done:
    if(iRet)
    {
        at_output("\r\n+PING:%u\r\nOK\r\n", g_tPingResult.avg_time);
    }
    else
    {
        at_output("\r\n+PING:TIMEOUT\r\nERROR\r\n");
    }
    CtrlWifi_PsStateForce(STA_PS_AWAKE_MODE, 0);
    return iRet;
}

void at_cmd_ping_callback_patch(ping_result_t *ptResult)
{
    if(ptResult)
    {
        memcpy((void *)&g_tPingResult, ptResult, sizeof(g_tPingResult));
    }

    g_u8PingCmdDone = 1;
    CtrlWifi_PsStateForce(STA_PS_NONE, 0);
    return;
}


/*-------------------------------------------------------------------------------------
 * Interface assignment
 *------------------------------------------------------------------------------------*/
void at_cmd_tcpip_api_preinit_patch(void)
{
    at_trans_client_init           = at_trans_client_init_patch;
    at_create_tcpip_tx_task        = at_create_tcpip_tx_task_patch;
    at_server_timeout_handler      = at_server_timeout_handler_patch;
    at_socket_process_task         = at_socket_process_task_patch;
    at_cmd_ping_callback           = at_cmd_ping_callback_patch;
    
    g_AtCmdTbl_Tcpip_Ptr[2].cmd_handle   = at_cmd_tcpip_cipstart_patch;
    g_AtCmdTbl_Tcpip_Ptr[3].cmd_handle   = at_cmd_tcpip_cipsend_patch;
    g_AtCmdTbl_Tcpip_Ptr[8].cmd_handle   = at_cmd_tcpip_cipserver_patch;
    g_AtCmdTbl_Tcpip_Ptr[10].cmd_handle  = at_cmd_tcpip_savetranslink_patch;
    g_AtCmdTbl_Tcpip_Ptr[19].cmd_handle  = at_cmd_tcpip_ping_patch;
}
