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
 *   - DapThread: USB bulk ↔ mailbox/mempool pipeline
 *   - UartThread: USB CDC ↔ I/O queue UART bridge
 *   - Main thread: event-driven LED with virtual timer
 *
 * Core 1 (ChibiOS RT SMP):
 *   - DapProcessThread: DAP command processing
 *   - SWD bit-banging via SIO registers
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
#define DAP_PROC_THREAD_WA_SIZE 512U
#define UART_THREAD_WA_SIZE     512U

/* UART bridge buffer size. */
#define UART_BRIDGE_BUF_SIZE    64U

/* Number of DAP packet buffers in memory pool. */
#define DAP_POOL_SIZE           4U

/* UART I/O queue size. */
#define UART_QUEUE_SIZE         256U

/*===========================================================================*/
/* RAMFUNC: Core 1 wait loop.                                                */
/*===========================================================================*/

/*
 * All functions that run with XIP disabled must be in RAM.
 * The .ramtext section is copied from flash to RAM by CRT0 at boot.
 */
#define RAMFUNC __attribute__((noinline, section(".ramtext")))

/**
 * @brief   Core 1 wait loop — stays in RAM while Core 0 reads flash.
 * @note    RAMFUNC — runs from RAM.
 */
RAMFUNC static void c1_wait_for_init(volatile uint32_t *flag) {
  while (*flag == 0U) {
    __WFE();
  }
}

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
/* Boot handshake flags for flash ID reading.                                */
/*===========================================================================*/

static volatile uint32_t c1_in_ram    = 0U;
static volatile uint32_t init_complete = 0U;

/*===========================================================================*/
/* Event sources.                                                            */
/*===========================================================================*/

event_source_t evt_usb;
static event_source_t evt_dap;
static event_source_t evt_uart;

/*===========================================================================*/
/* Memory pool and mailboxes for DAP command pipeline.                       */
/*===========================================================================*/

static dap_packet_t dap_packets[DAP_POOL_SIZE];
static memory_pool_t dap_pool;

static msg_t cmd_mbox_buf[DAP_POOL_SIZE];
static msg_t resp_mbox_buf[DAP_POOL_SIZE];
static mailbox_t cmd_mbox;
static mailbox_t resp_mbox;

/*===========================================================================*/
/* DAP state (accessed by DapThread on Core 0 and DapProcessThread on        */
/* Core 1 — only abort flag is cross-core, single-byte relaxed access).      */
/*===========================================================================*/

static dap_data_t dap_state;

/*===========================================================================*/
/* DAP endpoint USB buffers and synchronization.                             */
/*===========================================================================*/

static uint8_t dap_rx_buf[DAP_PACKET_SIZE];
static uint8_t dap_tx_buf[DAP_PACKET_SIZE];
static binary_semaphore_t dap_rx_sem;
static volatile uint32_t dap_rx_len;

/*===========================================================================*/
/* DAP USB callbacks (called from USB ISR context).                          */
/*===========================================================================*/

void dap_usb_out_cb(USBDriver *usbp, usbep_t ep) {
  dap_rx_len = usbGetReceiveTransactionSizeX(usbp, ep);
  chSysLockFromISR();
  chBSemSignalI(&dap_rx_sem);
  chSysUnlockFromISR();
}

void dap_usb_in_cb(USBDriver *usbp, usbep_t ep) {
  (void)usbp;
  (void)ep;
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
/* DapThread (Core 0) — USB ↔ mailbox bridge.                               */
/*===========================================================================*/

static THD_WORKING_AREA(waDapThread, DAP_THREAD_WA_SIZE);
static THD_FUNCTION(DapThread, arg) {
  (void)arg;

  while (true) {
    /* Wait for USB active. */
    while (USBD1.state != USB_ACTIVE)
      chThdSleepMilliseconds(100);

    /* Arm EP1 OUT to receive a DAP command. */
    chSysLock();
    usbStartReceiveI(&USBD1, DAP_EP, dap_rx_buf, DAP_PACKET_SIZE);
    chSysUnlock();

    /* Wait for USB OUT data. */
    chBSemWait(&dap_rx_sem);
    if (USBD1.state != USB_ACTIVE) continue;

    /* Handle Transfer Abort locally (must not queue). */
    if (dap_rx_buf[0] == DAP_CMD_TRANSFER_ABORT) {
      dap_state.abort = 1U;
      __DSB();
      dap_tx_buf[0] = DAP_CMD_TRANSFER_ABORT;
      dap_tx_buf[1] = DAP_OK;
      chSysLock();
      usbStartTransmitI(&USBD1, DAP_EP, dap_tx_buf, 2);
      chSysUnlock();
      continue;
    }

    /* Allocate packet from pool, fill command, post to mailbox. */
    dap_packet_t *pkt = chPoolAlloc(&dap_pool);
    if (pkt == NULL) continue;
    memcpy(pkt->cmd, dap_rx_buf, dap_rx_len);
    pkt->cmd_len = dap_rx_len;

    chMBPostTimeout(&cmd_mbox, (msg_t)pkt, TIME_INFINITE);

    /* Wait for response from DapProcessThread. */
    msg_t msg;
    chMBFetchTimeout(&resp_mbox, &msg, TIME_INFINITE);
    pkt = (dap_packet_t *)msg;

    /* Send response via USB. */
    memcpy(dap_tx_buf, pkt->resp, pkt->resp_len);
    chSysLock();
    usbStartTransmitI(&USBD1, DAP_EP, dap_tx_buf, pkt->resp_len);
    chSysUnlock();

    /* Return packet to pool. */
    chPoolFree(&dap_pool, pkt);
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
    msg_t msg;
    chMBFetchTimeout(&cmd_mbox, &msg, TIME_INFINITE);
    dap_packet_t *pkt = (dap_packet_t *)msg;

    pkt->resp_len = dap_process_command(&dap_state, pkt->cmd, pkt->resp);

    chMBPostTimeout(&resp_mbox, (msg_t)pkt, TIME_INFINITE);
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

  while (true) {
    chEvtWaitAnyTimeout(EVENT_MASK(0), TIME_MS2I(10));
    chEvtGetAndClearFlags(&el);

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
  /* Signal Core 0 that we're in RAM. */
  c1_in_ram = 1U;
  __DSB();
  __SEV();

  /* Wait in RAM while Core 0 reads flash unique ID. */
  c1_wait_for_init(&init_complete);

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
  uint8_t uid[8];
  char serial_hex[17];

  /*
   * halInit() launches Core 1 via start_core1() in hal_lld_init().
   * Core 1 enters c1_main(), signals c1_in_ram, then waits in
   * c1_wait_for_init (RAMFUNC) while we read the flash unique ID.
   */
  halInit();

  /* Wait for Core 1 to be safely in RAM before disabling XIP. */
  while (!c1_in_ram) { }
  __DSB();

  /* Read flash unique ID while Core 1 is safely in RAM. */
  efl_lld_read_unique_id(&EFLD1, uid);
  uid_to_hex(uid, serial_hex, 8U);

  dap_set_serial(serial_hex);
  usb_set_serial_string(serial_hex);

  /* Signal Core 1 that flash reading is complete. */
  init_complete = 1U;
  __DSB();
  __SEV();

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

  /* Memory pool and mailboxes for DAP command pipeline. */
  chPoolObjectInit(&dap_pool, sizeof(dap_packet_t), NULL);
  chPoolLoadArray(&dap_pool, dap_packets, DAP_POOL_SIZE);
  chMBObjectInit(&cmd_mbox, cmd_mbox_buf, DAP_POOL_SIZE);
  chMBObjectInit(&resp_mbox, resp_mbox_buf, DAP_POOL_SIZE);

  /* DAP receive semaphore. */
  chBSemObjectInit(&dap_rx_sem, true);

  /* Virtual timer for LED blink. */
  chVTObjectInit(&led_vt);

  /* I/O queues for UART bridge. */
  iqObjectInit(&uart_rx_iq, uart_rx_qbuf, UART_QUEUE_SIZE, NULL, NULL);
  oqObjectInit(&uart_tx_oq, uart_tx_qbuf, UART_QUEUE_SIZE,
               uart_tx_notify, NULL);

  /* Start the RTOS — Core 1 proceeds from chSysWaitSystemState(). */
  chSysInit();

  /* Initialize Serial-over-USB CDC driver (for UART bridge). */
  sduObjectInit(&SDU1);
  sduStart(&SDU1, &serusbcfg);

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
  chThdCreateStatic(waDapThread, sizeof(waDapThread),
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
