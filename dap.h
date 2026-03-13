/*
 * Copyright (C) 2026 Eric Molitor <github.com/emolitor>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * CMSIS-DAP v2 protocol handler for RP2040 debug probe.
 */

#ifndef DAP_H
#define DAP_H

#include <stdint.h>
#include "ch.h"

/*===========================================================================*/
/* DAP packet configuration.                                                 */
/*===========================================================================*/

#define DAP_PACKET_SIZE         64U
#define DAP_POOL_SIZE           16U
#define DAP_PACKET_COUNT        (DAP_POOL_SIZE - 2U)

/*===========================================================================*/
/* DAP Command IDs (CMSIS-DAP v2 specification).                             */
/*===========================================================================*/

#define DAP_CMD_INFO                    0x00U
#define DAP_CMD_HOST_STATUS             0x01U
#define DAP_CMD_CONNECT                 0x02U
#define DAP_CMD_DISCONNECT              0x03U
#define DAP_CMD_TRANSFER_CONFIGURE      0x04U
#define DAP_CMD_TRANSFER                0x05U
#define DAP_CMD_TRANSFER_BLOCK          0x06U
#define DAP_CMD_TRANSFER_ABORT          0x07U
#define DAP_CMD_WRITE_ABORT             0x08U
#define DAP_CMD_DELAY                   0x09U
#define DAP_CMD_RESET_TARGET            0x0AU
#define DAP_CMD_SWJ_PINS                0x10U
#define DAP_CMD_SWJ_CLOCK               0x11U
#define DAP_CMD_SWJ_SEQUENCE            0x12U
#define DAP_CMD_SWD_CONFIGURE           0x13U
#define DAP_CMD_SWD_SEQUENCE            0x1DU
#define DAP_CMD_JTAG_SEQUENCE           0x14U
#define DAP_CMD_JTAG_CONFIGURE          0x15U
#define DAP_CMD_JTAG_IDCODE             0x16U
#define DAP_CMD_SWO_TRANSPORT           0x17U
#define DAP_CMD_SWO_MODE                0x18U
#define DAP_CMD_SWO_BAUDRATE            0x19U
#define DAP_CMD_SWO_CONTROL             0x1AU
#define DAP_CMD_SWO_STATUS              0x1BU
#define DAP_CMD_SWO_EXTENDED_STATUS     0x1EU
#define DAP_CMD_SWO_DATA                0x1CU
#define DAP_CMD_UART_TRANSPORT          0x1FU
#define DAP_CMD_UART_CONFIGURE          0x20U
#define DAP_CMD_UART_CONTROL            0x21U
#define DAP_CMD_UART_STATUS             0x22U
#define DAP_CMD_UART_TRANSFER           0x23U

#define DAP_CMD_QUEUE_COMMANDS          0x7EU
#define DAP_CMD_EXECUTE_COMMANDS        0x7FU

/*===========================================================================*/
/* DAP_Info IDs.                                                             */
/*===========================================================================*/

#define DAP_INFO_VENDOR                 0x01U
#define DAP_INFO_PRODUCT                0x02U
#define DAP_INFO_SER_NUM                0x03U
#define DAP_INFO_CMSIS_DAP_VER          0x04U
#define DAP_INFO_DEVICE_VENDOR          0x05U
#define DAP_INFO_DEVICE_NAME            0x06U
#define DAP_INFO_BOARD_VENDOR           0x07U
#define DAP_INFO_BOARD_NAME             0x08U
#define DAP_INFO_FW_VER                 0x09U
#define DAP_INFO_CAPABILITIES           0xF0U
#define DAP_INFO_TEST_DOMAIN_TIMER      0xF1U
#define DAP_INFO_UART_RX_BUF_SIZE       0xFBU
#define DAP_INFO_UART_TX_BUF_SIZE       0xFCU
#define DAP_INFO_SWO_BUF_SIZE           0xFDU
#define DAP_INFO_PACKET_COUNT           0xFEU
#define DAP_INFO_PACKET_SIZE            0xFFU

/*===========================================================================*/
/* DAP status codes.                                                         */
/*===========================================================================*/

#define DAP_OK                  0x00U
#define DAP_ERROR               0xFFU

/*===========================================================================*/
/* DAP Connect port types.                                                   */
/*===========================================================================*/

#define DAP_PORT_DEFAULT        0x00U
#define DAP_PORT_SWD            0x01U
#define DAP_PORT_JTAG           0x02U

/*===========================================================================*/
/* DAP Transfer request bits.                                                */
/*===========================================================================*/

#define DAP_TRANSFER_APnDP      (1U << 0)
#define DAP_TRANSFER_RnW        (1U << 1)
#define DAP_TRANSFER_A2         (1U << 2)
#define DAP_TRANSFER_A3         (1U << 3)
#define DAP_TRANSFER_MATCH_VALUE (1U << 4)
#define DAP_TRANSFER_MATCH_MASK (1U << 5)
#define DAP_TRANSFER_TIMESTAMP  (1U << 7)

/*===========================================================================*/
/* SWJ Pins bit definitions.                                                 */
/*===========================================================================*/

#define DAP_SWJ_SWCLK_TCK      (1U << 0)
#define DAP_SWJ_SWDIO_TMS      (1U << 1)
#define DAP_SWJ_TDI            (1U << 2)
#define DAP_SWJ_TDO            (1U << 3)
#define DAP_SWJ_nTRST          (1U << 5)
#define DAP_SWJ_nRESET         (1U << 7)

/*===========================================================================*/
/* Capabilities byte.                                                        */
/*===========================================================================*/

/* Bit 0: SWD, Bit 4: Atomic Commands (ExecuteCommands/QueueCommands). */
#define DAP_CAP_SWD             ((1U << 0) | (1U << 4))

/*===========================================================================*/
/* DAP packet for memory pool allocation.                                    */
/*===========================================================================*/

typedef struct {
  uint8_t           cmd[DAP_PACKET_SIZE];
  uint8_t           resp[DAP_PACKET_SIZE];
  uint32_t          cmd_len;
  uint32_t          resp_len;
} dap_packet_t;

/*===========================================================================*/
/* DAP event flags (broadcast via evt_dap).                                  */
/*===========================================================================*/

#define EVT_DAP_CONNECTED               (1U << 0)
#define EVT_DAP_DISCONNECTED            (1U << 1)
#define EVT_DAP_RUNNING                 (1U << 2)
#define EVT_DAP_IDLE                    (1U << 3)

/*===========================================================================*/
/* DAP state structure.                                                      */
/*===========================================================================*/

typedef struct {
  uint8_t   debug_port;         /* 0=disconnected, 1=SWD, 2=JTAG */
  uint32_t  clk_div;            /* PIO clock divider (16.8 fixed-point) */
  uint32_t  clock_freq;         /* requested clock frequency in Hz */

  /* Transfer configuration. */
  uint8_t   idle_cycles;
  uint16_t  retry_count;
  uint16_t  match_retry;

  /* SWD configuration. */
  uint8_t   turnaround;         /* turnaround clock cycles (1-4) */
  uint8_t   data_phase;         /* data phase on WAIT/FAULT */

  /* Transfer match. */
  uint32_t  match_mask;

  /* Abort flag (set by DapThread via DAP_TransferAbort). */
  volatile uint8_t abort;

  /* Event source for broadcasting DAP state changes. */
  event_source_t *evt_dap;
} dap_data_t;

/*===========================================================================*/
/* Function prototypes.                                                      */
/*===========================================================================*/

void     dap_init(dap_data_t *dap);
void     dap_set_serial(const char *serial);
uint32_t dap_process_command(dap_data_t *dap, const uint8_t *request,
                              uint8_t *response);

#endif /* DAP_H */
