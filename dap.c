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
 * CMSIS-DAP v2 protocol handler for debug probe.
 *
 * Implements the command dispatcher and all required DAP commands.
 * Runs on Core 1 as a ChibiOS SMP thread.
 */

#include <string.h>
#include "ch.h"
#include "hal.h"
#include "dap.h"
#include "swd.h"

/*===========================================================================*/
/* DAP Info strings.                                                         */
/*===========================================================================*/

static const char dap_vendor[]      = "ChibiOS";
static const char dap_product[]     = "ChibiOS Probe (CMSIS-DAP)";
static char       dap_serial[17]    = "0000000000000000";
static const char dap_cmsis_ver[]   = "2.1.1";
static const char dap_fw_ver[]      = "1.0.0";

void dap_set_serial(const char *serial) {
  uint32_t i;

  for (i = 0U; i < 16U && serial[i] != '\0'; i++) {
    dap_serial[i] = serial[i];
  }
  dap_serial[i] = '\0';
}

/*===========================================================================*/
/* Helper: build SWD request byte from DAP transfer request.                 */
/*===========================================================================*/

static uint8_t swd_request_byte(uint32_t request) {
  uint8_t req = 0x81U;  /* Start bit = 1, Stop = 0, Park = 1 */

  if (request & DAP_TRANSFER_APnDP)
    req |= (1U << 1);  /* APnDP */
  if (request & DAP_TRANSFER_RnW)
    req |= (1U << 2);  /* RnW */
  if (request & DAP_TRANSFER_A2)
    req |= (1U << 3);  /* A[2] */
  if (request & DAP_TRANSFER_A3)
    req |= (1U << 4);  /* A[3] */

  /* Parity over APnDP, RnW, A2, A3. */
  uint32_t parity = ((req >> 1) ^ (req >> 2) ^ (req >> 3) ^ (req >> 4)) & 1U;
  req |= (uint8_t)(parity << 5);

  return req;
}

/*===========================================================================*/
/* Delay helper (ChibiOS thread sleep).                                      */
/*===========================================================================*/

static void delay_ms(uint32_t ms) {
  chThdSleepMilliseconds(ms);
}

/*===========================================================================*/
/* DAP_Info (0x00).                                                          */
/*===========================================================================*/

static uint32_t dap_info(const uint8_t *req, uint8_t *resp) {
  uint8_t id = req[1];
  const char *str = NULL;
  uint32_t len;

  resp[0] = DAP_CMD_INFO;

  switch (id) {
  case DAP_INFO_VENDOR:
    str = dap_vendor;
    break;
  case DAP_INFO_PRODUCT:
    str = dap_product;
    break;
  case DAP_INFO_SER_NUM:
    str = dap_serial;
    break;
  case DAP_INFO_CMSIS_DAP_VER:
    str = dap_cmsis_ver;
    break;
  case DAP_INFO_FW_VER:
    str = dap_fw_ver;
    break;
  case DAP_INFO_CAPABILITIES:
    resp[1] = 1U;  /* length */
    resp[2] = DAP_CAP_SWD;
    return 3U;
  case DAP_INFO_TEST_DOMAIN_TIMER:
    resp[1] = 4U;  /* length */
    /* No test domain timer — return 0. */
    resp[2] = 0U;
    resp[3] = 0U;
    resp[4] = 0U;
    resp[5] = 0U;
    return 6U;
  case DAP_INFO_PACKET_COUNT:
    resp[1] = 1U;
    resp[2] = DAP_PACKET_COUNT;
    return 3U;
  case DAP_INFO_PACKET_SIZE:
    resp[1] = 2U;  /* length */
    resp[2] = (uint8_t)(DAP_PACKET_SIZE & 0xFFU);
    resp[3] = (uint8_t)(DAP_PACKET_SIZE >> 8);
    return 4U;
  default:
    /* Unknown info ID. */
    resp[1] = 0U;
    return 2U;
  }

  if (str != NULL) {
    len = (uint32_t)strlen(str) + 1U;  /* Include NUL terminator */
    resp[1] = (uint8_t)len;
    memcpy(&resp[2], str, len);
    return 2U + len;
  }

  resp[1] = 0U;
  return 2U;
}

/*===========================================================================*/
/* DAP_HostStatus (0x01).                                                    */
/*===========================================================================*/

static uint32_t dap_host_status(dap_data_t *dap, const uint8_t *req,
                                uint8_t *resp) {
  uint8_t type   = req[1];
  uint8_t status = req[2];

  resp[0] = DAP_CMD_HOST_STATUS;
  resp[1] = DAP_OK;

  /* Broadcast LED state change as event flags. */
  if (dap->evt_dap != NULL) {
    if (type == 0U) {
      chEvtBroadcastFlags(dap->evt_dap,
                          status ? EVT_DAP_CONNECTED : EVT_DAP_DISCONNECTED);
    }
    else if (type == 1U) {
      chEvtBroadcastFlags(dap->evt_dap,
                          status ? EVT_DAP_RUNNING : EVT_DAP_IDLE);
    }
  }

  return 2U;
}

/*===========================================================================*/
/* DAP_Connect (0x02).                                                       */
/*===========================================================================*/

static uint32_t dap_connect(dap_data_t *dap, const uint8_t *req,
                             uint8_t *resp) {
  uint8_t port = req[1];

  resp[0] = DAP_CMD_CONNECT;

  if (port == DAP_PORT_DEFAULT || port == DAP_PORT_SWD) {
    swd_init(dap->clk_div);
    dap->debug_port = DAP_PORT_SWD;
    resp[1] = DAP_PORT_SWD;
  }
  else {
    /* JTAG not supported. */
    dap->debug_port = 0U;
    resp[1] = 0U;
  }

  return 2U;
}

/*===========================================================================*/
/* DAP_Disconnect (0x03).                                                    */
/*===========================================================================*/

static uint32_t dap_disconnect(dap_data_t *dap, uint8_t *resp) {
  swd_off();
  dap->debug_port = 0U;
  resp[0] = DAP_CMD_DISCONNECT;
  resp[1] = DAP_OK;
  return 2U;
}

/*===========================================================================*/
/* DAP_TransferConfigure (0x04).                                             */
/*===========================================================================*/

static uint32_t dap_transfer_configure(dap_data_t *dap, const uint8_t *req,
                                        uint8_t *resp) {
  dap->idle_cycles = req[1];
  dap->retry_count = (uint16_t)req[2] | ((uint16_t)req[3] << 8);
  dap->match_retry = (uint16_t)req[4] | ((uint16_t)req[5] << 8);

  resp[0] = DAP_CMD_TRANSFER_CONFIGURE;
  resp[1] = DAP_OK;
  return 2U;
}

/*===========================================================================*/
/* DAP_Transfer (0x05).                                                      */
/*===========================================================================*/

static uint32_t dap_transfer(dap_data_t *dap, const uint8_t *req,
                              uint8_t *resp) {
  uint32_t req_idx = 2U;  /* Skip command + DAP index */
  uint32_t req_count = req[req_idx++];
  uint32_t resp_idx = 3U;  /* Skip command, count, response */
  uint32_t transfer_count = 0U;
  uint32_t transfer_response = 0U;
  uint32_t post_read = 0U;   /* Outstanding AP read needs flush */
  uint32_t check_write = 0U; /* Last write needs verification */
  uint32_t request;
  uint32_t data;
  uint8_t  ack;
  uint32_t retry;
  uint32_t n;
  uint8_t  swd_req;
  uint8_t  rdbuff_req = swd_request_byte(DAP_TRANSFER_RnW | (0x03U << 2));

  resp[0] = DAP_CMD_TRANSFER;

  for (n = 0; n < req_count; n++) {
    request = req[req_idx++];

    /* Check for abort. */
    if (dap->abort) {
      dap->abort = 0U;
      break;
    }

    if (request & DAP_TRANSFER_MATCH_MASK) {
      /* Set match mask. */
      dap->match_mask = (uint32_t)req[req_idx] |
                        ((uint32_t)req[req_idx + 1] << 8) |
                        ((uint32_t)req[req_idx + 2] << 16) |
                        ((uint32_t)req[req_idx + 3] << 24);
      req_idx += 4U;
      transfer_response = DAP_OK;
      transfer_count++;
      continue;
    }

    swd_req = swd_request_byte(request);

    if (request & DAP_TRANSFER_RnW) {
      /* Read transfer. */
      if (request & DAP_TRANSFER_MATCH_VALUE) {
        /* Read with match — flush any pending posted read first. */
        if (post_read) {
          for (retry = 0; retry <= dap->retry_count; retry++) {
            ack = swd_transfer(rdbuff_req, &data, dap->clk_div,
                               dap->idle_cycles, dap->turnaround,
                               dap->data_phase);
            if (ack != SWD_ACK_WAIT)
              break;
          }
          if (ack != SWD_ACK_OK) {
            transfer_response = ack;
            break;
          }
          post_read = 0U;
        }

        uint32_t match_val = (uint32_t)req[req_idx] |
                             ((uint32_t)req[req_idx + 1] << 8) |
                             ((uint32_t)req[req_idx + 2] << 16) |
                             ((uint32_t)req[req_idx + 3] << 24);
        req_idx += 4U;

        uint32_t match_retry = dap->match_retry;
        if (request & DAP_TRANSFER_APnDP) {
          /* AP read: issue dummy read to prime pipeline. */
          for (retry = 0; retry <= dap->retry_count; retry++) {
            ack = swd_transfer(swd_req, NULL, dap->clk_div,
                               dap->idle_cycles, dap->turnaround,
                               dap->data_phase);
            if (ack != SWD_ACK_WAIT)
              break;
          }
          if (ack != SWD_ACK_OK) {
            transfer_response = ack;
            break;
          }
        }

        /* Retry reads until match or timeout. */
        do {
          uint8_t read_req;
          if (request & DAP_TRANSFER_APnDP)
            read_req = rdbuff_req;
          else
            read_req = swd_req;

          for (retry = 0; retry <= dap->retry_count; retry++) {
            ack = swd_transfer(read_req, &data, dap->clk_div,
                               dap->idle_cycles, dap->turnaround,
                               dap->data_phase);
            if (ack != SWD_ACK_WAIT)
              break;
          }
          if (ack != SWD_ACK_OK) {
            transfer_response = ack;
            break;
          }
        } while (((data & dap->match_mask) != match_val) && match_retry-- && !dap->abort);

        if ((data & dap->match_mask) != match_val) {
          transfer_response = (uint32_t)ack | (1U << 4); /* MISMATCH flag */
        }
        else {
          transfer_response = ack;
        }
        transfer_count++;

        if (transfer_response != DAP_OK)
          break;

        check_write = 0U;
      }
      else {
        /* Normal read. */

        /* Flush pending posted read before this read if needed. */
        if (post_read) {
          if ((request & DAP_TRANSFER_APnDP) && !(request & DAP_TRANSFER_MATCH_VALUE)) {
            /* Next is also an AP read — pipeline: this read returns prev data. */
            for (retry = 0; retry <= dap->retry_count; retry++) {
              ack = swd_transfer(swd_req, &data, dap->clk_div,
                                 dap->idle_cycles, dap->turnaround,
                                 dap->data_phase);
              if (ack != SWD_ACK_WAIT)
                break;
            }
          }
          else {
            /* Not an AP read — flush via RDBUFF. */
            for (retry = 0; retry <= dap->retry_count; retry++) {
              ack = swd_transfer(rdbuff_req, &data, dap->clk_div,
                                 dap->idle_cycles, dap->turnaround,
                                 dap->data_phase);
              if (ack != SWD_ACK_WAIT)
                break;
            }
            post_read = 0U;
          }
          if (ack != SWD_ACK_OK) {
            transfer_response = ack;
            transfer_count++;
            break;
          }

          /* Store the previous posted read's data. */
          resp[resp_idx++] = (uint8_t)(data);
          resp[resp_idx++] = (uint8_t)(data >> 8);
          resp[resp_idx++] = (uint8_t)(data >> 16);
          resp[resp_idx++] = (uint8_t)(data >> 24);
          transfer_response = ack;
          check_write = 0U;

          /* If we already pipelined via AP read, post_read stays set. */
          if (!(request & DAP_TRANSFER_APnDP)) {
            /* DP read: issue and store directly. */
            for (retry = 0; retry <= dap->retry_count; retry++) {
              ack = swd_transfer(swd_req, &data, dap->clk_div,
                                 dap->idle_cycles, dap->turnaround,
                                 dap->data_phase);
              if (ack != SWD_ACK_WAIT)
                break;
            }
            if (ack != SWD_ACK_OK) {
              transfer_response = ack;
              transfer_count++;
              break;
            }
            resp[resp_idx++] = (uint8_t)(data);
            resp[resp_idx++] = (uint8_t)(data >> 8);
            resp[resp_idx++] = (uint8_t)(data >> 16);
            resp[resp_idx++] = (uint8_t)(data >> 24);
            transfer_response = ack;
            transfer_count++;
          }
          else {
            /* AP read was pipelined, post_read remains 1. */
            transfer_count++;
          }
        }
        else {
          /* No pending posted read. */
          for (retry = 0; retry <= dap->retry_count; retry++) {
            ack = swd_transfer(swd_req, &data, dap->clk_div,
                               dap->idle_cycles, dap->turnaround,
                               dap->data_phase);
            if (ack != SWD_ACK_WAIT)
              break;
          }
          if (ack != SWD_ACK_OK) {
            transfer_response = ack;
            transfer_count++;
            break;
          }

          if (request & DAP_TRANSFER_APnDP) {
            /* AP read: this is a dummy read to prime pipeline. */
            post_read = 1U;
          }
          else {
            /* DP read: data is immediately available. */
            resp[resp_idx++] = (uint8_t)(data);
            resp[resp_idx++] = (uint8_t)(data >> 8);
            resp[resp_idx++] = (uint8_t)(data >> 16);
            resp[resp_idx++] = (uint8_t)(data >> 24);
          }
          transfer_response = ack;
          transfer_count++;
          check_write = 0U;
        }
      }
    }
    else {
      /* Write transfer. */

      /* Flush pending posted read before write. */
      if (post_read) {
        for (retry = 0; retry <= dap->retry_count; retry++) {
          ack = swd_transfer(rdbuff_req, &data, dap->clk_div,
                             dap->idle_cycles, dap->turnaround,
                             dap->data_phase);
          if (ack != SWD_ACK_WAIT)
            break;
        }
        if (ack != SWD_ACK_OK) {
          transfer_response = ack;
          transfer_count++;
          break;
        }
        /* Store the flushed AP read data. */
        resp[resp_idx++] = (uint8_t)(data);
        resp[resp_idx++] = (uint8_t)(data >> 8);
        resp[resp_idx++] = (uint8_t)(data >> 16);
        resp[resp_idx++] = (uint8_t)(data >> 24);
        transfer_response = ack;
        transfer_count++;
        post_read = 0U;
      }

      data = (uint32_t)req[req_idx] |
             ((uint32_t)req[req_idx + 1] << 8) |
             ((uint32_t)req[req_idx + 2] << 16) |
             ((uint32_t)req[req_idx + 3] << 24);
      req_idx += 4U;

      for (retry = 0; retry <= dap->retry_count; retry++) {
        ack = swd_transfer(swd_req, &data, dap->clk_div,
                           dap->idle_cycles, dap->turnaround,
                           dap->data_phase);
        if (ack != SWD_ACK_WAIT)
          break;
      }
      if (ack != SWD_ACK_OK) {
        transfer_response = ack;
        transfer_count++;
        break;
      }

      transfer_response = ack;
      transfer_count++;
      check_write = 1U;
    }
  }

  /* Flush any pending posted AP read at end of sequence. */
  if (post_read) {
    for (retry = 0; retry <= dap->retry_count; retry++) {
      ack = swd_transfer(rdbuff_req, &data, dap->clk_div,
                         dap->idle_cycles, dap->turnaround,
                         dap->data_phase);
      if (ack != SWD_ACK_WAIT)
        break;
    }
    if (ack == SWD_ACK_OK) {
      resp[resp_idx++] = (uint8_t)(data);
      resp[resp_idx++] = (uint8_t)(data >> 8);
      resp[resp_idx++] = (uint8_t)(data >> 16);
      resp[resp_idx++] = (uint8_t)(data >> 24);
    }
    else {
      transfer_response = ack;
    }
  }
  else if (check_write) {
    /* Verify last write didn't cause a fault. */
    for (retry = 0; retry <= dap->retry_count; retry++) {
      ack = swd_transfer(rdbuff_req, NULL, dap->clk_div,
                         dap->idle_cycles, dap->turnaround,
                         dap->data_phase);
      if (ack != SWD_ACK_WAIT)
        break;
    }
    if (ack != SWD_ACK_OK) {
      transfer_response = ack;
    }
  }

  resp[1] = (uint8_t)transfer_count;
  resp[2] = (uint8_t)transfer_response;
  return resp_idx;
}

/*===========================================================================*/
/* DAP_TransferBlock (0x06).                                                 */
/*===========================================================================*/

static uint32_t dap_transfer_block(dap_data_t *dap, const uint8_t *req,
                                    uint8_t *resp) {
  uint32_t req_idx = 2U;  /* Skip command + DAP index */
  uint32_t count = (uint32_t)req[req_idx] | ((uint32_t)req[req_idx + 1] << 8);
  req_idx += 2U;
  uint32_t request = req[req_idx++];
  uint32_t resp_idx = 4U;  /* Skip command, count(2), response */
  uint32_t transfer_count = 0U;
  uint32_t data;
  uint8_t  ack = 0U;
  uint8_t  swd_req;
  uint32_t retry;
  uint32_t n;

  resp[0] = DAP_CMD_TRANSFER_BLOCK;

  swd_req = swd_request_byte(request);

  if (request & DAP_TRANSFER_RnW) {
    /* Block read. */
    if (request & DAP_TRANSFER_APnDP) {
      /* AP read: issue dummy read first to prime pipeline. */
      for (retry = 0; retry <= dap->retry_count; retry++) {
        ack = swd_transfer(swd_req, NULL, dap->clk_div,
                           dap->idle_cycles, dap->turnaround,
                           dap->data_phase);
        if (ack != SWD_ACK_WAIT)
          break;
      }
      if (ack != SWD_ACK_OK)
        goto block_done;
    }

    for (n = 0; n < count; n++) {
      if (dap->abort) {
        dap->abort = 0U;
        break;
      }

      uint8_t read_req;
      if ((request & DAP_TRANSFER_APnDP) && (n == count - 1U))
        read_req = swd_request_byte(DAP_TRANSFER_RnW | (0x03U << 2)); /* RDBUFF */
      else
        read_req = swd_req;

      for (retry = 0; retry <= dap->retry_count; retry++) {
        ack = swd_transfer(read_req, &data, dap->clk_div,
                           dap->idle_cycles, dap->turnaround,
                           dap->data_phase);
        if (ack != SWD_ACK_WAIT)
          break;
      }
      if (ack != SWD_ACK_OK)
        break;

      resp[resp_idx++] = (uint8_t)(data);
      resp[resp_idx++] = (uint8_t)(data >> 8);
      resp[resp_idx++] = (uint8_t)(data >> 16);
      resp[resp_idx++] = (uint8_t)(data >> 24);
      transfer_count++;
    }
  }
  else {
    /* Block write. */
    for (n = 0; n < count; n++) {
      if (dap->abort) {
        dap->abort = 0U;
        break;
      }

      data = (uint32_t)req[req_idx] |
             ((uint32_t)req[req_idx + 1] << 8) |
             ((uint32_t)req[req_idx + 2] << 16) |
             ((uint32_t)req[req_idx + 3] << 24);
      req_idx += 4U;

      for (retry = 0; retry <= dap->retry_count; retry++) {
        ack = swd_transfer(swd_req, &data, dap->clk_div,
                           dap->idle_cycles, dap->turnaround,
                           dap->data_phase);
        if (ack != SWD_ACK_WAIT)
          break;
      }
      if (ack != SWD_ACK_OK)
        break;
      transfer_count++;
    }

    /* Check last write result by reading RDBUFF. */
    if (ack == SWD_ACK_OK) {
      uint8_t rdbuff_req = swd_request_byte(DAP_TRANSFER_RnW | (0x03U << 2));
      for (retry = 0; retry <= dap->retry_count; retry++) {
        ack = swd_transfer(rdbuff_req, NULL, dap->clk_div,
                           dap->idle_cycles, dap->turnaround,
                           dap->data_phase);
        if (ack != SWD_ACK_WAIT)
          break;
      }
    }
  }

block_done:
  resp[1] = (uint8_t)(transfer_count);
  resp[2] = (uint8_t)(transfer_count >> 8);
  resp[3] = (uint8_t)ack;
  return resp_idx;
}

/*===========================================================================*/
/* DAP_WriteABORT (0x08).                                                    */
/*===========================================================================*/

static uint32_t dap_write_abort(dap_data_t *dap, const uint8_t *req,
                                 uint8_t *resp) {
  /* Write to DP ABORT register (DP, Write, A[3:2]=0). */
  uint32_t data;
  uint8_t swd_req;
  uint8_t ack;
  uint32_t retry;

  resp[0] = DAP_CMD_WRITE_ABORT;

  data = (uint32_t)req[2] |
         ((uint32_t)req[3] << 8) |
         ((uint32_t)req[4] << 16) |
         ((uint32_t)req[5] << 24);

  swd_req = swd_request_byte(0U);  /* DP, Write, A[3:2]=0 → ABORT reg */

  for (retry = 0; retry <= dap->retry_count; retry++) {
    ack = swd_transfer(swd_req, &data, dap->clk_div,
                       dap->idle_cycles, dap->turnaround,
                       dap->data_phase);
    if (ack != SWD_ACK_WAIT)
      break;
  }

  resp[1] = (ack == SWD_ACK_OK) ? DAP_OK : DAP_ERROR;
  return 2U;
}

/*===========================================================================*/
/* DAP_SWJ_Pins (0x10).                                                     */
/*===========================================================================*/

static uint32_t dap_swj_pins(const uint8_t *req, uint8_t *resp) {
  uint8_t pin_output = req[1];
  uint8_t pin_select = req[2];
  uint32_t wait_us = (uint32_t)req[3] |
                     ((uint32_t)req[4] << 8) |
                     ((uint32_t)req[5] << 16) |
                     ((uint32_t)req[6] << 24);

  resp[0] = DAP_CMD_SWJ_PINS;

  /* Apply selected pin outputs. */
  if (pin_select & DAP_SWJ_SWCLK_TCK) {
    if (pin_output & DAP_SWJ_SWCLK_TCK)
      SIO_GPIO_OUT_SET = SWCLK_BIT;
    else
      SIO_GPIO_OUT_CLR = SWCLK_BIT;
  }
  if (pin_select & DAP_SWJ_SWDIO_TMS) {
    if (pin_output & DAP_SWJ_SWDIO_TMS)
      SIO_GPIO_OUT_SET = SWDIO_BIT;
    else
      SIO_GPIO_OUT_CLR = SWDIO_BIT;
  }
  if (pin_select & DAP_SWJ_nRESET) {
    if (pin_output & DAP_SWJ_nRESET) {
      /* nRESET deasserted = OE off (pull-up pulls high). */
      SIO_GPIO_OE_CLR = NRESET_BIT;
    }
    else {
      /* nRESET asserted = OE on (output value is 0). */
      SIO_GPIO_OE_SET = NRESET_BIT;
    }
  }

  /* Wait for specified time, if any. */
  if (wait_us > 0U) {
    delay_ms((wait_us + 999U) / 1000U);
  }

  /* Read pin state. */
  uint32_t gpio_in = SIO_GPIO_IN;
  uint8_t pins = 0U;
  if (gpio_in & SWCLK_BIT)
    pins |= DAP_SWJ_SWCLK_TCK;
  if (gpio_in & SWDIO_BIT)
    pins |= DAP_SWJ_SWDIO_TMS;
  if (gpio_in & NRESET_BIT)
    pins |= DAP_SWJ_nRESET;

  resp[1] = pins;
  return 2U;
}

/*===========================================================================*/
/* DAP_SWJ_Clock (0x11).                                                     */
/*===========================================================================*/

static uint32_t dap_swj_clock(dap_data_t *dap, const uint8_t *req,
                               uint8_t *resp) {
  uint32_t clock = (uint32_t)req[1] |
                   ((uint32_t)req[2] << 8) |
                   ((uint32_t)req[3] << 16) |
                   ((uint32_t)req[4] << 24);

  resp[0] = DAP_CMD_SWJ_CLOCK;

  if (clock == 0U) {
    resp[1] = DAP_ERROR;
    return 2U;
  }

  dap->clock_freq = clock;

  /* Calculate PIO clock divider as 16.8 fixed-point.
   * PIO SM runs at sys_clk / clkdiv, each SWCLK period = 4 PIO cycles.
   * clkdiv = sys_clk / (4 * target_freq).
   * Minimum clkdiv = 1.0 (encoded as 0x100). */
  uint32_t clkdiv_256 = (uint32_t)((uint64_t)RP_CLK_SYS_FREQ * 64U / clock);
  if (clkdiv_256 < 0x100U)
    clkdiv_256 = 0x100U;  /* Minimum 1.0 */
  dap->clk_div = clkdiv_256;
  if (dap->debug_port == DAP_PORT_SWD)
    swd_set_clkdiv(clkdiv_256);

  resp[1] = DAP_OK;
  return 2U;
}

/*===========================================================================*/
/* DAP_SWJ_Sequence (0x12).                                                  */
/*===========================================================================*/

static uint32_t dap_swj_sequence(dap_data_t *dap, const uint8_t *req,
                                  uint8_t *resp) {
  uint32_t count = req[1];
  if (count == 0U)
    count = 256U;

  resp[0] = DAP_CMD_SWJ_SEQUENCE;

  swj_sequence(count, &req[2], dap->clk_div);

  resp[1] = DAP_OK;
  return 2U;
}

/*===========================================================================*/
/* DAP_SWD_Configure (0x13).                                                 */
/*===========================================================================*/

static uint32_t dap_swd_configure(dap_data_t *dap, const uint8_t *req,
                                   uint8_t *resp) {
  uint8_t cfg = req[1];

  dap->turnaround = (cfg & 0x03U) + 1U;
  dap->data_phase = (cfg >> 2) & 0x01U;

  resp[0] = DAP_CMD_SWD_CONFIGURE;
  resp[1] = DAP_OK;
  return 2U;
}

/*===========================================================================*/
/* DAP_SWD_Sequence (0x1D).                                                  */
/*===========================================================================*/

static uint32_t dap_swd_sequence(dap_data_t *dap, const uint8_t *req,
                                  uint8_t *resp) {
  uint32_t req_idx = 1U;
  uint32_t resp_idx = 2U;
  uint32_t seq_count = req[req_idx++];
  uint32_t n;

  resp[0] = DAP_CMD_SWD_SEQUENCE;
  resp[1] = DAP_OK;

  for (n = 0; n < seq_count; n++) {
    uint32_t info = req[req_idx++];
    uint32_t count = info & 0x3FU;
    if (count == 0U)
      count = 64U;
    uint32_t bytes = (count + 7U) >> 3;

    if (info & 0x80U) {
      /* Input: capture bits to response. */
      swd_sequence(info, NULL, &resp[resp_idx], dap->clk_div);
      resp_idx += bytes;
    }
    else {
      /* Output: send bits from request. */
      swd_sequence(info, &req[req_idx], NULL, dap->clk_div);
      req_idx += bytes;
    }
  }

  return resp_idx;
}

/*===========================================================================*/
/* DAP_Delay (0x09).                                                         */
/*===========================================================================*/

static uint32_t dap_delay(const uint8_t *req, uint8_t *resp) {
  uint32_t delay = (uint32_t)req[1] | ((uint32_t)req[2] << 8);

  resp[0] = DAP_CMD_DELAY;
  delay_ms(delay);
  resp[1] = DAP_OK;
  return 2U;
}

/*===========================================================================*/
/* DAP_ResetTarget (0x0A).                                                   */
/*===========================================================================*/

static uint32_t dap_reset_target(uint8_t *resp) {
  resp[0] = DAP_CMD_RESET_TARGET;
  resp[1] = DAP_OK;
  resp[2] = 0U;  /* No device-specific reset sequence */
  return 3U;
}

/*===========================================================================*/
/* Public API.                                                               */
/*===========================================================================*/

void dap_init(dap_data_t *dap) {
  memset(dap, 0, sizeof(dap_data_t));
  /* Default 1 MHz: clkdiv = sys_clk / (4 * 1e6) as 16.8 fixed-point. */
  dap->clock_freq   = 1000000U;
  dap->clk_div      = (uint32_t)((uint64_t)RP_CLK_SYS_FREQ * 64U / 1000000U);
  dap->idle_cycles  = 0U;
  dap->retry_count  = 100U;
  dap->match_retry  = 0U;
  dap->turnaround   = 1U;
  dap->data_phase   = 0U;
  dap->match_mask   = 0xFFFFFFFFU;
}

uint32_t dap_process_command(dap_data_t *dap, const uint8_t *request,
                              uint8_t *response) {
  uint8_t cmd = request[0];

  switch (cmd) {
  case DAP_CMD_INFO:
    return dap_info(request, response);

  case DAP_CMD_HOST_STATUS:
    return dap_host_status(dap, request, response);

  case DAP_CMD_CONNECT:
    return dap_connect(dap, request, response);

  case DAP_CMD_DISCONNECT:
    return dap_disconnect(dap, response);

  case DAP_CMD_TRANSFER_CONFIGURE:
    return dap_transfer_configure(dap, request, response);

  case DAP_CMD_TRANSFER:
    return dap_transfer(dap, request, response);

  case DAP_CMD_TRANSFER_BLOCK:
    return dap_transfer_block(dap, request, response);

  case DAP_CMD_TRANSFER_ABORT:
    /* Abort is handled by Core 0 setting dap->abort flag. */
    response[0] = DAP_CMD_TRANSFER_ABORT;
    response[1] = DAP_OK;
    return 2U;

  case DAP_CMD_WRITE_ABORT:
    return dap_write_abort(dap, request, response);

  case DAP_CMD_DELAY:
    return dap_delay(request, response);

  case DAP_CMD_RESET_TARGET:
    return dap_reset_target(response);

  case DAP_CMD_SWJ_PINS:
    return dap_swj_pins(request, response);

  case DAP_CMD_SWJ_CLOCK:
    return dap_swj_clock(dap, request, response);

  case DAP_CMD_SWJ_SEQUENCE:
    return dap_swj_sequence(dap, request, response);

  case DAP_CMD_SWD_CONFIGURE:
    return dap_swd_configure(dap, request, response);

  case DAP_CMD_SWD_SEQUENCE:
    return dap_swd_sequence(dap, request, response);

  /* Unsupported commands return DAP_ERROR. */
  case DAP_CMD_JTAG_SEQUENCE:
  case DAP_CMD_JTAG_CONFIGURE:
  case DAP_CMD_JTAG_IDCODE:
  case DAP_CMD_SWO_TRANSPORT:
  case DAP_CMD_SWO_MODE:
  case DAP_CMD_SWO_BAUDRATE:
  case DAP_CMD_SWO_CONTROL:
  case DAP_CMD_SWO_STATUS:
  case DAP_CMD_SWO_EXTENDED_STATUS:
  case DAP_CMD_SWO_DATA:
  case DAP_CMD_UART_TRANSPORT:
  case DAP_CMD_UART_CONFIGURE:
  case DAP_CMD_UART_CONTROL:
  case DAP_CMD_UART_STATUS:
  case DAP_CMD_UART_TRANSFER:
    response[0] = cmd;
    response[1] = DAP_ERROR;
    return 2U;

  default:
    response[0] = cmd;
    response[1] = DAP_ERROR;
    return 2U;
  }
}
