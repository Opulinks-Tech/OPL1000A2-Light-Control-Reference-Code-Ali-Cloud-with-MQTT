/* *****************************************************************************
 *  Copyright 2019, Opulinks Technology Ltd.
 *  ---------------------------------------------------------------------------
 *  Statement:
 *  ----------
 *  This software is protected by Copyright and the information contained
 *  herein is confidential. The software may not be copied and the information
 *  contained herein may not be used or disclosed except with the written
 *  permission of Opulinks Technology Ltd. (C) 2018
 *
 *******************************************************************************
 *
 *  @file hal_dbg_uart_patch.h 
 * 
 *  @brief Patch for APS DBG_UART API patch
 *  
 ******************************************************************************/

#ifndef _HAL_DBG_UART_PATCH_H_
#define _HAL_DBG_UART_PATCH_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 *************************************************************************
 *                          Include files
 *************************************************************************
 */
#include "hal_dbg_uart.h"
/*
 *************************************************************************
 *                          Definitions and Macros
 *************************************************************************
 */


/*
 *************************************************************************
 *                          Typedefs and Structures
 *************************************************************************
 */



/*
 *************************************************************************
 *                          Public Variables
 *************************************************************************
 */


/*
 *************************************************************************
 *                          Public Functions
 *************************************************************************
 */
void Hal_DbgUart_PatchInit(void);
void Hal_DbgUart_DividerUpdate(void);


#ifdef __cplusplus
}
#endif
#endif  /* _HAL_DBG_UART_PATCH_H_ */
