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
 * CMSIS-DAP v2 Debug Probe — RP2040/RP2350 Dual-Core SMP Implementation
 *
 * Core 0 runtime:
 *   - Main thread: hardware init, USB bring-up, LED state machine
 *   - DapThread: pipelined USB bulk ↔ object FIFO/mailbox pipeline
 *   - UartThread: USB CDC ↔ I/O queue UART bridge
 *
 * Core 1 runtime:
 *   - DapProcessThread: DAP command processing
 *
 * SWD transfers execute from DapProcessThread via the PIO state machine.
 *
 * Pin layout (matching Pico Debug Probe):
 *   GPIO 1: nRESET (open-drain)
 *   GPIO 2: SWCLK (output)
 *   GPIO 3: SWDIO (bidirectional)
 *   GPIO 4: UART TX (to target RX)
 *   GPIO 5: UART RX (from target TX)
 *   GPIO 25: LED (onboard)
 */

#include <string.h>

#include "ch.h"
#include "hal.h"

#include "usbcfg.h"
#include "dap.h"
#include "swd.h"

/*===========================================================================*/
/* Configuration.                                                            */
/*===========================================================================*/

#define LED_PIN                 25U
#define UART_TX_PIN             4U
#define UART_RX_PIN             5U

#define DAP_THREAD_WA_SIZE      512U
#define DAP_PROC_THREAD_WA_SIZE 1024U
#define UART_THREAD_WA_SIZE     512U

/* UART bridge buffer size. */
#define UART_BRIDGE_BUF_SIZE    64U

/* UART I/O queue size. */
#define UART_QUEUE_SIZE         256U

/*===========================================================================*/
/* Hex conversion helper.                                                    */
/*===========================================================================*/

static const char hex_chars[] = "0123456789ABCDEF";

static void uid_to_hex(const uint8_t *uid, char *hex, uint32_t len) {
  uint32_t i;

  for (i = 0U; i < len; i++) {
    hex[i * 2U]      = hex_chars[(uid[i] >> 4) & 0x0FU];
    hex[i * 2U + 1U] = hex_chars[uid[i] & 0x0FU];
  }
  hex[len * 2U] = '\0';
}

/*===========================================================================*/
/* Early flash unique ID read (runs before main, before Core 1 starts).      */
/* USB serial string initialization.                                         */
/*===========================================================================*/

/**
 * @brief   CRT0 late init hook — runs after BSS/DATA init, before main().
 */
void __late_init(void) {
  uint8_t uid[4U + RP_FLASH_UNIQUE_ID_SIZE] = {0U};
  char serial_hex[RP_FLASH_UNIQUE_ID_SIZE * 2U + 1U];
  flash_error_t err;

  /* Prepare any platform-specific RAM state needed before XIP is toggled. */
  /* This is idempotent on RP devices. */
  rp_efl_lld_init();

  /*
   * The low-level implementation masks and restores interrupts using the
   * appropriate ARM or Hazard3 mechanism while XIP is disabled.
   */
  err = rp_efl_lld_read_uid_full(&EFLD1, uid, sizeof(uid));

  /* A failed read may have partially written the buffer, so clear it to a
   * deterministic all-zero serial instead of publishing stale bytes. */
  if (err != FLASH_NO_ERROR)
    memset(uid, 0, sizeof(uid));

  uid_to_hex(uid + 4U, serial_hex, RP_FLASH_UNIQUE_ID_SIZE);
  dap_set_serial(serial_hex);
  usb_set_serial_string(serial_hex);
}

/*===========================================================================*/
/* Event sources.                                                            */
/*===========================================================================*/

event_source_t evt_usb;
static event_source_t evt_dap;
static event_source_t evt_uart;

/*===========================================================================*/
/* Object FIFO and mailbox for DAP command pipeline.                         */
/*===========================================================================*/

static dap_packet_t dap_packets[DAP_POOL_SIZE];
static objects_fifo_t cmd_fifo;
static msg_t cmd_fifo_buf[DAP_POOL_SIZE];
static msg_t resp_mbox_buf[DAP_POOL_SIZE];
static mailbox_t resp_mbox;

/*===========================================================================*/
/* DAP state.                                                                */
/*===========================================================================*/

static dap_data_t dap_state;

/*===========================================================================*/
/* DapThread reference and USB receive state.                                */
/*===========================================================================*/

static thread_t *dap_thd;
static volatile uint32_t dap_rx_len;
static volatile bool dap_transport_active;
static uint32_t inflight;

/* Thread event masks for DapThread. */
#define EVT_DAP_RX_DONE         EVENT_MASK(0)
#define EVT_DAP_TX_DONE         EVENT_MASK(1)
#define EVT_DAP_RESP_READY      EVENT_MASK(2)

/*===========================================================================*/
/* DAP USB callbacks (called from USB ISR context).                          */
/*===========================================================================*/

void dap_usb_out_cb(USBDriver *usbp, usbep_t ep) {
  dap_rx_len = usbGetReceiveTransactionSizeX(usbp, ep);
  chSysLockFromISR();
  chEvtSignalI(dap_thd, EVT_DAP_RX_DONE);
  chSysUnlockFromISR();
}

void dap_usb_in_cb(USBDriver *usbp, usbep_t ep) {
  (void)usbp;
  (void)ep;
  chSysLockFromISR();
  chEvtSignalI(dap_thd, EVT_DAP_TX_DONE);
  chSysUnlockFromISR();
}

/*===========================================================================*/
/* Virtual timer and LED state tracking.                                     */
/*===========================================================================*/

static virtual_timer_t led_vt;
static bool dap_connected;
static bool dap_running;

/**
 * @brief   Virtual timer callback — toggles LED (ISR context).
 */
static void led_timer_cb(virtual_timer_t *vtp, void *p) {
  (void)vtp; (void)p;
  palToggleLine(LED_PIN);
}

/**
 * @brief   Reconfigure LED based on current state.
 */
static void led_update(void) {
  chVTReset(&led_vt);
  if (dap_running) {
    chVTSetContinuous(&led_vt, TIME_MS2I(500), led_timer_cb, NULL);
  }
  else if (dap_connected) {
    palSetLine(LED_PIN);
  }
  else {
    palClearLine(LED_PIN);
  }
}

/*===========================================================================*/
/* DapThread (Core 0) — pipelined USB ↔ object FIFO bridge.                 */
/*===========================================================================*/

static THD_WORKING_AREA(waDapThread, DAP_THREAD_WA_SIZE);
static THD_FUNCTION(DapThread, arg) {
  (void)arg;
  dap_packet_t *rx_pkt = NULL;
  dap_packet_t *tx_pkt = NULL;
  dap_packet_t *queued[DAP_PACKET_COUNT];
  uint32_t queued_count = 0U;
  /* Deferred TransferAbort responses. An abort is handled on Core 0 and is
   * never queued to the worker, so its response is injected into the outgoing
   * stream at its request position: abort_at[k] is the running response count
   * at which it must be sent (every worker response that precedes it). */
  dap_packet_t *abort_pkt[DAP_PACKET_COUNT];
  uint32_t abort_at[DAP_PACKET_COUNT];
  uint32_t abort_head = 0U;
  uint32_t abort_count = 0U;
  uint32_t responses_sent = 0U;

  while (true) {
    /* Wait for USB active. */
    while (USBD1.state != USB_ACTIVE)
      chThdSleepMilliseconds(100);

    dap_transport_active = true;
    rx_pkt = NULL;
    responses_sent = 0U;
    abort_head = 0U;
    abort_count = 0U;

    /* Arm OUT immediately (the pool is full at connection start) so the first
     * request is serviced without waiting out an event-loop timeout. */
    rx_pkt = chFifoTakeObjectTimeout(&cmd_fifo, TIME_IMMEDIATE);
    if (rx_pkt != NULL) {
      chSysLock();
      usbStartReceiveI(&USBD1, DAP_EP, rx_pkt->cmd, DAP_PACKET_SIZE);
      chSysUnlock();
    }

    while (USBD1.state == USB_ACTIVE) {
      eventmask_t events = chEvtWaitAnyTimeout(
          EVT_DAP_RX_DONE | EVT_DAP_TX_DONE | EVT_DAP_RESP_READY,
          TIME_MS2I(100));

      /* Free a completed TX buffer. */
      if ((events & EVT_DAP_TX_DONE) && (tx_pkt != NULL)) {
        chFifoReturnObject(&cmd_fifo, tx_pkt);
        tx_pkt = NULL;
      }

      /* Process a received command (rx_pkt was armed and is now filled). */
      if ((events & EVT_DAP_RX_DONE) && (rx_pkt != NULL)) {
        uint32_t i;

        rx_pkt->cmd_len = dap_rx_len;

        if ((rx_pkt->cmd_len > 0U) &&
            (rx_pkt->cmd[0] == DAP_CMD_QUEUE_COMMANDS) &&
            (queued_count < DAP_PACKET_COUNT)) {
          /* DAP_QueueCommands has no immediate response. Keep ownership of
           * the packet until the first following non-queue command. */
          queued[queued_count++] = rx_pkt;
        }
        else {
          bool is_abort = (rx_pkt->cmd_len > 0U) &&
                          (rx_pkt->cmd[0] == DAP_CMD_TRANSFER_ABORT);
          bool overflow = (rx_pkt->cmd_len > 0U) &&
                          (rx_pkt->cmd[0] == DAP_CMD_QUEUE_COMMANDS);

          /* An abort interrupts the transfer currently executing on the
           * worker: a stuck transfer polls dap_state.abort and bails. Queued
           * commands committed alongside still execute so the host receives a
           * response for each — matching TransferAbort's "abort the current
           * transfer" semantics rather than dropping the batch and desyncing
           * the host's response count. */
          if (is_abort) {
            dap_state.abort = 1U;
            __DSB();
          }

          /* A non-queue packet — or a QueueCommands that overflowed the pool
           * — commits the atomic batch. Transform each retained packet into
           * ExecuteCommands (response ID 0x7F) and submit it ahead of the
           * triggering command. Committing on overflow, rather than
           * discarding, keeps a response flowing for every packet sent. */
          for (i = 0U; i < queued_count; i++) {
            queued[i]->cmd[0] = DAP_CMD_EXECUTE_COMMANDS;
            chFifoSendObject(&cmd_fifo, queued[i]);
            inflight++;
          }
          queued_count = 0U;

          if (is_abort && (abort_count < DAP_PACKET_COUNT)) {
            /* Defer the abort response to its request position: it is sent
             * once responses_sent reaches the number of responses that
             * precede it (everything committed above plus anything already in
             * flight). This keeps the outgoing stream ordered without a
             * synchronous drain. */
            uint32_t slot = (abort_head + abort_count) % DAP_PACKET_COUNT;
            rx_pkt->resp[0] = DAP_CMD_TRANSFER_ABORT;
            rx_pkt->resp[1] = DAP_OK;
            abort_pkt[slot] = rx_pkt;
            abort_at[slot] = responses_sent + inflight;
            abort_count++;
          }
          else if (overflow) {
            /* Reject the overflowing packet with a one-byte error; the batch
             * it could not join was committed above. */
            rx_pkt->cmd[0] = DAP_ERROR;
            rx_pkt->cmd_len = 1U;
            chFifoSendObject(&cmd_fifo, rx_pkt);
            inflight++;
          }
          else {
            chFifoSendObject(&cmd_fifo, rx_pkt);
            inflight++;
          }
        }

        rx_pkt = NULL;  /* Consumed; re-armed below once a buffer is free. */
      }

      /* Transmit the next outgoing packet when the endpoint is idle. Worker
       * responses drain in FIFO (request) order; a deferred abort response is
       * emitted exactly when every response preceding it has been sent. */
      if (tx_pkt == NULL) {
        msg_t msg;
        if ((abort_count > 0U) && (abort_at[abort_head] <= responses_sent)) {
          dap_packet_t *ap = abort_pkt[abort_head];
          abort_head = (abort_head + 1U) % DAP_PACKET_COUNT;
          abort_count--;
          chSysLock();
          usbStartTransmitI(&USBD1, DAP_EP, ap->resp, 2U);
          chSysUnlock();
          tx_pkt = ap;
        }
        else if (chMBFetchTimeout(&resp_mbox, &msg,
                                  TIME_IMMEDIATE) == MSG_OK) {
          dap_packet_t *pkt = (dap_packet_t *)msg;
          if (inflight > 0U)
            inflight--;
          responses_sent++;
          chSysLock();
          usbStartTransmitI(&USBD1, DAP_EP, pkt->resp, pkt->resp_len);
          chSysUnlock();
          tx_pkt = pkt;
        }
      }

      /* Re-arm USB OUT when unarmed and a buffer is available. If the pool is
       * momentarily exhausted the endpoint stays unarmed and the host is
       * NAKed (flow control) until a response drains and frees a buffer — the
       * thread never blocks, so it always observes USB teardown. */
      if (rx_pkt == NULL) {
        rx_pkt = chFifoTakeObjectTimeout(&cmd_fifo, TIME_IMMEDIATE);
        if (rx_pkt != NULL) {
          chSysLock();
          usbStartReceiveI(&USBD1, DAP_EP, rx_pkt->cmd, DAP_PACKET_SIZE);
          chSysUnlock();
        }
      }
    }

    /* USB disconnected — drain in-flight work and return every buffer. */
    {
      msg_t msg;
      uint32_t i;
      void *objp;

      dap_transport_active = false;
      dap_state.abort = 1U;
      __DSB();

      /* Reclaim commands the worker has not fetched. */
      while (chFifoReceiveObjectTimeout(&cmd_fifo, &objp,
                                        TIME_IMMEDIATE) == MSG_OK) {
        chFifoReturnObject(&cmd_fifo, objp);
        if (inflight > 0U)
          inflight--;
      }

      while (chMBFetchTimeout(&resp_mbox, &msg, TIME_IMMEDIATE) == MSG_OK) {
        chFifoReturnObject(&cmd_fifo, (dap_packet_t *)msg);
        if (inflight > 0U)
          inflight--;
      }

      /* At most one command was executing; wait for its final response. */
      while (inflight > 0U) {
        if (chMBFetchTimeout(&resp_mbox, &msg,
                             TIME_INFINITE) == MSG_OK) {
          chFifoReturnObject(&cmd_fifo, (dap_packet_t *)msg);
          inflight--;
        }
      }

      for (i = 0U; i < queued_count; i++)
        chFifoReturnObject(&cmd_fifo, queued[i]);
      queued_count = 0U;

      while (abort_count > 0U) {
        chFifoReturnObject(&cmd_fifo, abort_pkt[abort_head]);
        abort_head = (abort_head + 1U) % DAP_PACKET_COUNT;
        abort_count--;
      }
      inflight = 0U;
    }
    if (rx_pkt != NULL) {
      chFifoReturnObject(&cmd_fifo, rx_pkt);
      rx_pkt = NULL;
    }
    if (tx_pkt != NULL) {
      chFifoReturnObject(&cmd_fifo, tx_pkt);
      tx_pkt = NULL;
    }
  }
}

/*===========================================================================*/
/* DapProcessThread (Core 1) — DAP command processor.                        */
/*===========================================================================*/

static THD_WORKING_AREA(waDapProcessThread, DAP_PROC_THREAD_WA_SIZE);
static THD_FUNCTION(DapProcessThread, arg) {
  (void)arg;

  dap_init(&dap_state);
  dap_state.evt_dap = &evt_dap;

  while (true) {
    void *objp;
    if (chFifoReceiveObjectTimeout(&cmd_fifo, &objp,
                                   TIME_INFINITE) != MSG_OK)
      continue;
    dap_packet_t *pkt = (dap_packet_t *)objp;
    dap_process_result_t result;

    dap_state.abort = 0U;
    if (dap_transport_active) {
      result = dap_process_command(&dap_state, pkt->cmd, pkt->cmd_len,
                                   pkt->resp, sizeof(pkt->resp));
    }
    else {
      pkt->resp[0] = DAP_ERROR;
      result.status = DAP_PROCESS_MALFORMED;
      result.response_len = 1U;
    }
    pkt->resp_len = result.response_len;

    chMBPostTimeout(&resp_mbox, (msg_t)pkt, TIME_INFINITE);
    chEvtSignal(dap_thd, EVT_DAP_RESP_READY);
  }
}

/*===========================================================================*/
/* UART bridge I/O queues and SIO callback.                                  */
/*===========================================================================*/

static uint8_t uart_rx_qbuf[UART_QUEUE_SIZE];
static input_queue_t uart_rx_iq;

/**
 * @brief   SIO event callback — drains RX FIFO into input queue.
 * @note    Called from ISR context by sio_lld_serve_interrupt.
 */
static void uart_sio_cb(SIODriver *siop) {
  osalSysLockFromISR();

  while (!sioIsRXEmptyX(siop)) {
    msg_t b = sioGetX(siop);
    iqPutI(&uart_rx_iq, (uint8_t)b);
  }
  chEvtBroadcastFlagsI(&evt_uart, 1U);

  osalSysUnlockFromISR();
}

/*===========================================================================*/
/* UART1 configuration for bridge.                                           */
/*===========================================================================*/

static SIOConfig uart_bridge_config = {
  .baud      = 115200,
  .UARTLCR_H = UART_UARTLCR_H_WLEN_8BITS | UART_UARTLCR_H_FEN,
  .UARTCR    = 0,
  .UARTIFLS  = UART_UARTIFLS_RXIFLSEL_1_8F | UART_UARTIFLS_TXIFLSEL_1_8E,
  .UARTDMACR = 0
};

static void uart_apply_linecoding(const cdc_linecoding_t *lc) {
  uint32_t baud = (uint32_t)lc->dwDTERate[0]       |
                  ((uint32_t)lc->dwDTERate[1] << 8) |
                  ((uint32_t)lc->dwDTERate[2] << 16)|
                  ((uint32_t)lc->dwDTERate[3] << 24);
  if (baud == 0U)
    return;

  uint32_t lcr_h = UART_UARTLCR_H_FEN;

  switch (lc->bDataBits) {
  case 5:  lcr_h |= UART_UARTLCR_H_WLEN_5BITS; break;
  case 6:  lcr_h |= UART_UARTLCR_H_WLEN_6BITS; break;
  case 7:  lcr_h |= UART_UARTLCR_H_WLEN_7BITS; break;
  default: lcr_h |= UART_UARTLCR_H_WLEN_8BITS; break;
  }

  if (lc->bCharFormat == LC_STOP_2)
    lcr_h |= UART_UARTLCR_H_STP2;

  if (lc->bParityType == LC_PARITY_ODD)
    lcr_h |= UART_UARTLCR_H_PEN;
  else if (lc->bParityType == LC_PARITY_EVEN)
    lcr_h |= UART_UARTLCR_H_PEN | UART_UARTLCR_H_EPS;

  uart_bridge_config.baud = baud;
  uart_bridge_config.UARTLCR_H = lcr_h;
  sioStart(&SIOD1, &uart_bridge_config);
  sioWriteEnableFlags(&SIOD1, SIO_EV_RXNOTEMPY | SIO_EV_RXIDLE);
}

/*===========================================================================*/
/* UartThread (Core 0) — I/O queue UART bridge.                              */
/*===========================================================================*/

static THD_WORKING_AREA(waUartThread, UART_THREAD_WA_SIZE);
static THD_FUNCTION(UartThread, arg) {
  (void)arg;
  uint8_t buf[UART_BRIDGE_BUF_SIZE];

  event_listener_t el;
  chEvtRegisterMask(&evt_uart, &el, EVENT_MASK(0));

  event_listener_t el_sdu;
  chEvtRegisterMaskWithFlags(chnGetEventSource(&SDU1), &el_sdu,
                             EVENT_MASK(1), CHN_INPUT_AVAILABLE);

  while (true) {
    chEvtWaitAnyTimeout(EVENT_MASK(0) | EVENT_MASK(1), TIME_MS2I(10));
    chEvtGetAndClearFlags(&el);
    chEvtGetAndClearFlags(&el_sdu);

    /* Reconfigure if line coding changed. */
    if (usb_linecoding_changed()) {
      cdc_linecoding_t lc;
      usb_get_linecoding(&lc);
      uart_apply_linecoding(&lc);
    }

    /* UART → USB: drain input queue into SDU1. */
    size_t n = iqReadTimeout(&uart_rx_iq, buf, sizeof(buf), TIME_IMMEDIATE);
    if (n > 0U)
      chnWriteTimeout(&SDU1, buf, n, TIME_MS2I(100));

    /* USB → UART: read SDU1 and write directly to SIO TX FIFO. */
    n = chnReadTimeout(&SDU1, buf, sizeof(buf), TIME_IMMEDIATE);
    while (n > 0U) {
      size_t written = sioAsyncWrite(&SIOD1, buf, n);
      n -= written;
      if (n > 0U) {
        memmove(buf, buf + written, n);
        chThdSleepMilliseconds(1);
      }
    }
  }
}

/*===========================================================================*/
/* Core 1 entry point.                                                       */
/*===========================================================================*/

void c1_main(void) {
  /* Proceed with ChibiOS SMP initialization. */
  chSysWaitSystemState(ch_sys_running);
  chInstanceObjectInit(&ch1, &ch_core1_cfg);
  chSysUnlock();

  /* Create DAP processing thread on Core 1. */
  chThdCreateStatic(waDapProcessThread, sizeof(waDapProcessThread),
                    NORMALPRIO + 2, DapProcessThread, NULL);

  /* Core 1 main thread idle loop. */
  while (true) {
    chThdSleepMilliseconds(1000);
  }
}

/*===========================================================================*/
/* Main thread.                                                              */
/*===========================================================================*/

int main(void) {
  halInit();

  /*
   * Initialize all shared kernel objects before chSysInit().
   */

  /* Event sources. */
  chEvtObjectInit(&evt_usb);
  chEvtObjectInit(&evt_dap);
  chEvtObjectInit(&evt_uart);

  /* Object FIFO for DAP command pipeline.
   * Pool and mailbox structures are initialized here; the pool is loaded
   * after chSysInit() so that chPoolLoadArray() can take kernel locks. */
  chPoolObjectInit(&cmd_fifo.free.pool, sizeof(dap_packet_t), NULL);
  chSemObjectInit(&cmd_fifo.free.sem, (cnt_t)DAP_POOL_SIZE);
  chMBObjectInit(&cmd_fifo.mbx, cmd_fifo_buf, DAP_POOL_SIZE);
  chMBObjectInit(&resp_mbox, resp_mbox_buf, DAP_POOL_SIZE);

  /* Virtual timer for LED blink. */
  chVTObjectInit(&led_vt);

  /* Input queue for UART bridge RX. */
  iqObjectInit(&uart_rx_iq, uart_rx_qbuf, UART_QUEUE_SIZE, NULL, NULL);

  /* Start the RTOS.  Core 1 proceeds from chSysWaitSystemState(). */
  chSysInit();

  /* Load pool objects now that kernel locks are available. */
  chPoolLoadArray(&cmd_fifo.free.pool, dap_packets, DAP_POOL_SIZE);

  /* Initialize Serial-over-USB CDC driver (for UART bridge). */
  sduObjectInit(&SDU1);
  sduStart(&SDU1, &serusbcfg);

  /* Start worker threads before exposing USB so endpoint callbacks always
   * have valid thread targets as soon as the host starts probing. */
  dap_thd = chThdCreateStatic(waDapThread, sizeof(waDapThread),
                               NORMALPRIO + 1, DapThread, NULL);
  chThdCreateStatic(waUartThread, sizeof(waUartThread),
                    NORMALPRIO, UartThread, NULL);

  /* Start USB with disconnect/reconnect pattern. */
  usbDisconnectBus(serusbcfg.usbp);
  chThdSleepMilliseconds(1500);
  usbStart(serusbcfg.usbp, &usbcfg);
  usbConnectBus(serusbcfg.usbp);

  /* Configure UART1 pins and start SIO. */
  palSetPadMode(IOPORT1, UART_TX_PIN, PAL_MODE_ALTERNATE_UART);
  palSetPadMode(IOPORT1, UART_RX_PIN, PAL_MODE_ALTERNATE_UART);
  sioStart(&SIOD1, &uart_bridge_config);
  SIOD1.cb = uart_sio_cb;
  sioWriteEnableFlags(&SIOD1, SIO_EV_RXNOTEMPY | SIO_EV_RXIDLE);

  /* Configure LED pin. */
  palSetLineMode(LED_PIN, PAL_MODE_OUTPUT_PUSHPULL);

  /* Register for USB and DAP events. */
  event_listener_t el_usb, el_dap;
  chEvtRegisterMaskWithFlags(&evt_usb, &el_usb, EVENT_MASK(0),
      EVT_USB_CONFIGURED | EVT_USB_RESET |
      EVT_USB_SUSPENDED | EVT_USB_WAKEUP);
  chEvtRegisterMaskWithFlags(&evt_dap, &el_dap, EVENT_MASK(1),
      EVT_DAP_CONNECTED | EVT_DAP_DISCONNECTED |
      EVT_DAP_RUNNING | EVT_DAP_IDLE);

  /* Main thread: event-driven LED status. */
  while (true) {
    eventmask_t events = chEvtWaitAny(EVENT_MASK(0) | EVENT_MASK(1));

    if (events & EVENT_MASK(0))
      chEvtGetAndClearFlags(&el_usb);
    if (events & EVENT_MASK(1)) {
      eventflags_t flags = chEvtGetAndClearFlags(&el_dap);
      if (flags & EVT_DAP_CONNECTED)
        dap_connected = true;
      if (flags & EVT_DAP_DISCONNECTED) {
        dap_connected = false;
        dap_running = false;
      }
      if (flags & EVT_DAP_RUNNING)
        dap_running = true;
      if (flags & EVT_DAP_IDLE)
        dap_running = false;
    }

    led_update();
  }

  return 0;
}
