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
 * CMSIS-DAP v2 Debug Probe — RP2040 Dual-Core SMP Implementation
 *
 * Core 0 (ChibiOS RT SMP):
 *   - USB composite device (CMSIS-DAP v2 + CDC ACM)
 *   - DapThread: pipelined USB bulk ↔ object FIFO/mailbox pipeline
 *   - UartThread: USB CDC ↔ I/O queue UART bridge
 *   - Main thread: event-driven LED with virtual timer
 *
 * Core 1 (ChibiOS RT SMP):
 *   - DapProcessThread: DAP command processing
 *   - SWD via PIO state machine (deterministic timing)
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
#include "rtt.h"

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
/*===========================================================================*/

/**
 * @brief   CRT0 late init hook — runs after BSS/DATA init, before main().
 */
void __late_init(void) {
  uint8_t uid[8];
  char serial_hex[17];

  efl_lld_read_unique_id(&EFLD1, uid);
  uid_to_hex(uid, serial_hex, 8U);

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
/* DAP state (accessed by DapThread on Core 0 and DapProcessThread on        */
/* Core 1 — only abort flag is cross-core, single-byte relaxed access).      */
/*===========================================================================*/

static dap_data_t dap_state;

/*===========================================================================*/
/* DapThread reference and USB receive state.                                */
/*===========================================================================*/

static thread_t *dap_thd;
static volatile uint32_t dap_rx_len;
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
static bool usb_active;
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
  dap_packet_t *rx_pkt;
  dap_packet_t *tx_pkt = NULL;

  while (true) {
    /* Wait for USB active. */
    while (USBD1.state != USB_ACTIVE)
      chThdSleepMilliseconds(100);

    /* Allocate initial RX buffer and arm USB OUT. */
    rx_pkt = chFifoTakeObjectTimeout(&cmd_fifo, TIME_INFINITE);
    chSysLock();
    usbStartReceiveI(&USBD1, DAP_EP, rx_pkt->cmd, DAP_PACKET_SIZE);
    chSysUnlock();

    while (USBD1.state == USB_ACTIVE) {
      eventmask_t events = chEvtWaitAnyTimeout(
          EVT_DAP_RX_DONE | EVT_DAP_TX_DONE | EVT_DAP_RESP_READY,
          TIME_MS2I(100));

      /* Free completed TX buffer. */
      if (events & EVT_DAP_TX_DONE) {
        chFifoReturnObject(&cmd_fifo, tx_pkt);
        tx_pkt = NULL;
      }

      /* Process received command. */
      if (events & EVT_DAP_RX_DONE) {
        rx_pkt->cmd_len = dap_rx_len;

        if (rx_pkt->cmd[0] == DAP_CMD_TRANSFER_ABORT) {
          /* Handle abort locally — must not queue to Core 1. */
          dap_state.abort = 1U;
          __DSB();

          /* Wait for in-flight TX to complete. */
          if (tx_pkt != NULL) {
            chEvtWaitAny(EVT_DAP_TX_DONE);
            chFifoReturnObject(&cmd_fifo, tx_pkt);
            tx_pkt = NULL;
          }

          /* Drain all in-flight responses before sending abort response.
           * Host expects responses in order: aborted command(s) first,
           * then abort response last. */
          while (inflight > 0U && USBD1.state == USB_ACTIVE) {
            msg_t msg;
            if (chMBFetchTimeout(&resp_mbox, &msg,
                                 TIME_MS2I(200)) == MSG_OK) {
              dap_packet_t *pkt = (dap_packet_t *)msg;
              inflight--;
              chSysLock();
              usbStartTransmitI(&USBD1, DAP_EP, pkt->resp, pkt->resp_len);
              chSysUnlock();
              chEvtWaitAny(EVT_DAP_TX_DONE);
              chFifoReturnObject(&cmd_fifo, pkt);
            }
            else {
              break;
            }
          }

          /* Send abort response last. */
          rx_pkt->resp[0] = DAP_CMD_TRANSFER_ABORT;
          rx_pkt->resp[1] = DAP_OK;
          chSysLock();
          usbStartTransmitI(&USBD1, DAP_EP, rx_pkt->resp, 2U);
          chSysUnlock();
          tx_pkt = rx_pkt;
        }
        else {
          /* Send command to Core 1 via object FIFO. */
          chFifoSendObject(&cmd_fifo, rx_pkt);
          inflight++;
        }

        /* Allocate next RX buffer and re-arm USB OUT. */
        rx_pkt = chFifoTakeObjectTimeout(&cmd_fifo, TIME_INFINITE);
        chSysLock();
        usbStartReceiveI(&USBD1, DAP_EP, rx_pkt->cmd, DAP_PACKET_SIZE);
        chSysUnlock();
      }

      /* Start TX if idle and response available. */
      if (tx_pkt == NULL) {
        msg_t msg;
        if (chMBFetchTimeout(&resp_mbox, &msg, TIME_IMMEDIATE) == MSG_OK) {
          dap_packet_t *pkt = (dap_packet_t *)msg;
          inflight--;
          chSysLock();
          usbStartTransmitI(&USBD1, DAP_EP, pkt->resp, pkt->resp_len);
          chSysUnlock();
          tx_pkt = pkt;
        }
      }
    }

    /* USB disconnected — drain any leaked responses and return buffers. */
    {
      msg_t msg;
      while (chMBFetchTimeout(&resp_mbox, &msg, TIME_IMMEDIATE) == MSG_OK)
        chFifoReturnObject(&cmd_fifo, (dap_packet_t *)msg);
      inflight = 0U;
    }
    chFifoReturnObject(&cmd_fifo, rx_pkt);
    if (tx_pkt != NULL) {
      chFifoReturnObject(&cmd_fifo, tx_pkt);
      tx_pkt = NULL;
    }
  }
}

/*===========================================================================*/
/* DapProcessThread (Core 1) — DAP command processor.                        */
/*===========================================================================*/

static rtt_state_t rtt;

static THD_WORKING_AREA(waDapProcessThread, DAP_PROC_THREAD_WA_SIZE);
static THD_FUNCTION(DapProcessThread, arg) {
  (void)arg;

  dap_init(&dap_state);
  dap_state.evt_dap = &evt_dap;
  rtt_init(&rtt);

  systime_t rtt_last_poll = chVTGetSystemTimeX();

  while (true) {
    bool rtt_active = rtt_cdc_dtr_active();

    void *objp;
    msg_t status = chFifoReceiveObjectTimeout(&cmd_fifo, &objp,
                                               TIME_MS2I(rtt.poll_interval_ms));

    if (status == MSG_OK) {
      /* Process DAP command. */
      dap_packet_t *pkt = (dap_packet_t *)objp;
      pkt->resp_len = dap_process_command(&dap_state, pkt->cmd, pkt->resp);
      chMBPostTimeout(&resp_mbox, (msg_t)pkt, TIME_INFINITE);
      chEvtSignal(dap_thd, EVT_DAP_RESP_READY);
    }
    else if (rtt_active && dap_state.debug_port == DAP_PORT_SWD) {
      /* RTT poll: only when no DAP commands pending (FIFO timeout).
       * Must not run between host commands — RTT modifies DP SELECT
       * which corrupts the host's cached register state. */
      systime_t now = chVTGetSystemTimeX();
      if (chVTTimeElapsedSinceX(rtt_last_poll) >=
          TIME_MS2I(rtt.poll_interval_ms)) {
        rtt_last_poll = now;

        static uint8_t rtt_buf[RTT_READ_MAX];
        uint32_t n = rtt_poll(&rtt, dap_state.debug_port,
                              dap_state.idle_cycles,
                              dap_state.turnaround, dap_state.data_phase,
                              rtt_buf, sizeof(rtt_buf));

        if (n > 0U) {
          chnWriteTimeout(&SDU2, rtt_buf, (size_t)n, TIME_MS2I(10));
          if (rtt.poll_interval_ms > RTT_MIN_POLL_MS)
            rtt.poll_interval_ms /= 2U;
        }
        else {
          if (rtt.found && rtt.poll_interval_ms < RTT_MAX_POLL_MS)
            rtt.poll_interval_ms *= 2U;
        }

        /* Check for down-channel data from host. */
        static uint8_t down_buf[32];
        size_t dn = chnReadTimeout(&SDU2, down_buf, sizeof(down_buf),
                                    TIME_IMMEDIATE);
        if (dn > 0U)
          rtt_write_down(&rtt, dap_state.idle_cycles,
                          dap_state.turnaround, dap_state.data_phase,
                          down_buf, (uint32_t)dn);
      }
    }
  }
}

/*===========================================================================*/
/* UART bridge I/O queues and SIO callback.                                  */
/*===========================================================================*/

static uint8_t uart_rx_qbuf[UART_QUEUE_SIZE];
static uint8_t uart_tx_qbuf[UART_QUEUE_SIZE];
static input_queue_t uart_rx_iq;
static output_queue_t uart_tx_oq;

/**
 * @brief   Output queue notify — data enqueued, enable SIO TX interrupt.
 * @note    Called from locked thread context.
 */
static void uart_tx_notify(io_queue_t *qp) {
  (void)qp;
  sioSetEnableFlagsX(&SIOD1, SIO_EV_TXNOTFULL);
}

/**
 * @brief   SIO event callback — drains/fills FIFOs via I/O queues.
 * @note    Called from ISR context by sio_lld_serve_interrupt.
 */
static void uart_sio_cb(SIODriver *siop) {
  osalSysLockFromISR();

  /* Drain RX FIFO into input queue. */
  bool got_rx = false;
  while (!sioIsRXEmptyX(siop)) {
    msg_t b = sioGetX(siop);
    iqPutI(&uart_rx_iq, (uint8_t)b);
    got_rx = true;
  }
  if (got_rx)
    chEvtBroadcastFlagsI(&evt_uart, 1U);

  /* Fill TX FIFO from output queue. */
  while (!sioIsTXFullX(siop)) {
    msg_t b = oqGetI(&uart_tx_oq);
    if (b < MSG_OK) {
      /* Queue empty — disable TX interrupt. */
      sioClearEnableFlagsX(siop, SIO_EV_TXNOTFULL);
      break;
    }
    sioPutX(siop, (uint8_t)b);
  }

  osalSysUnlockFromISR();
}

/*===========================================================================*/
/* UART1 configuration for bridge.                                           */
/*===========================================================================*/

static const SIOConfig uart_bridge_config = {
  .baud      = 115200,
  .UARTLCR_H = UART_UARTLCR_H_WLEN_8BITS | UART_UARTLCR_H_FEN,
  .UARTCR    = 0,
  .UARTIFLS  = UART_UARTIFLS_RXIFLSEL_1_8F | UART_UARTIFLS_TXIFLSEL_1_8E,
  .UARTDMACR = 0
};

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

    /* UART → USB: drain input queue into SDU1. */
    size_t n = iqReadTimeout(&uart_rx_iq, buf, sizeof(buf), TIME_IMMEDIATE);
    if (n > 0U)
      chnWriteTimeout(&SDU1, buf, n, TIME_MS2I(100));

    /* USB → UART: read SDU1 into output queue. */
    n = chnReadTimeout(&SDU1, buf, sizeof(buf), TIME_IMMEDIATE);
    if (n > 0U)
      oqWriteTimeout(&uart_tx_oq, buf, n, TIME_MS2I(100));
  }
}

/*===========================================================================*/
/* Core 1 — ChibiOS SMP instance.                                           */
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
/* Core 0 — main (ChibiOS RT SMP).                                          */
/*===========================================================================*/

int main(void) {
  halInit();

  /*
   * Initialize all shared kernel objects before chSysInit().
   * Core 1 will proceed through chSysWaitSystemState() once
   * chSysInit() sets ch_sys_running, so everything must be
   * ready before that.
   */

  /* Event sources. */
  chEvtObjectInit(&evt_usb);
  chEvtObjectInit(&evt_dap);
  chEvtObjectInit(&evt_uart);

  /* Object FIFO for DAP command pipeline.
   * Initialize components manually because chFifoObjectInit() calls
   * chGuardedPoolLoadArray() which uses chGuardedPoolFree() — that
   * acquires the SMP kernel spinlock, which isn't available before
   * chSysInit(). Use plain chPoolLoadArray() + direct sem init instead. */
  chPoolObjectInit(&cmd_fifo.free.pool, sizeof(dap_packet_t), NULL);
  chSemObjectInit(&cmd_fifo.free.sem, (cnt_t)DAP_POOL_SIZE);
  chPoolLoadArray(&cmd_fifo.free.pool, dap_packets, DAP_POOL_SIZE);
  chMBObjectInit(&cmd_fifo.mbx, cmd_fifo_buf, DAP_POOL_SIZE);
  chMBObjectInit(&resp_mbox, resp_mbox_buf, DAP_POOL_SIZE);

  /* Virtual timer for LED blink. */
  chVTObjectInit(&led_vt);

  /* I/O queues for UART bridge. */
  iqObjectInit(&uart_rx_iq, uart_rx_qbuf, UART_QUEUE_SIZE, NULL, NULL);
  oqObjectInit(&uart_tx_oq, uart_tx_qbuf, UART_QUEUE_SIZE,
               uart_tx_notify, NULL);

  /* Start the RTOS — Core 1 proceeds from chSysWaitSystemState(). */
  chSysInit();

  /* Initialize Serial-over-USB CDC drivers. */
  sduObjectInit(&SDU1);
  sduStart(&SDU1, &serusbcfg);
  sduObjectInit(&SDU2);
  sduStart(&SDU2, &rtt_serusbcfg);

  /* Start USB with disconnect/reconnect pattern. */
  usbDisconnectBus(serusbcfg.usbp);
  chThdSleepMilliseconds(1500);
  usbStart(serusbcfg.usbp, &usbcfg);
  usbConnectBus(serusbcfg.usbp);

  /* Configure UART1 pins and start SIO with callback. */
  palSetPadMode(IOPORT1, UART_TX_PIN, PAL_MODE_ALTERNATE_UART);
  palSetPadMode(IOPORT1, UART_RX_PIN, PAL_MODE_ALTERNATE_UART);
  sioStart(&SIOD1, &uart_bridge_config);
  SIOD1.cb = uart_sio_cb;
  sioWriteEnableFlags(&SIOD1, SIO_EV_RXNOTEMPY);

  /* Configure LED pin. */
  palSetLineMode(LED_PIN, PAL_MODE_OUTPUT_PUSHPULL);

  /* Start DAP thread (higher priority). */
  dap_thd = chThdCreateStatic(waDapThread, sizeof(waDapThread),
                               NORMALPRIO + 1, DapThread, NULL);

  /* Start UART bridge thread. */
  chThdCreateStatic(waUartThread, sizeof(waUartThread),
                    NORMALPRIO, UartThread, NULL);

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

    if (events & EVENT_MASK(0)) {
      eventflags_t flags = chEvtGetAndClearFlags(&el_usb);
      if (flags & EVT_USB_CONFIGURED)
        usb_active = true;
      if (flags & (EVT_USB_RESET | EVT_USB_SUSPENDED))
        usb_active = false;
      if (flags & EVT_USB_WAKEUP)
        usb_active = true;
    }
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
