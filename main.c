/*
 * CMSIS-DAP v2 Debug Probe — RP2040 Dual-Core Implementation
 *
 * Core 0 (ChibiOS RT):
 *   - USB composite device (CMSIS-DAP v2 + CDC ACM)
 *   - DAP thread: USB bulk ↔ shared memory ↔ Core 1
 *   - UART bridge thread: SDU1 (USB CDC) ↔ SIOD1 (UART1)
 *   - LED status thread
 *
 * Core 1 (bare-metal):
 *   - DAP command processor (dap_process_command)
 *   - SWD bit-banging via SIO registers
 *   - Sleeps via WFE when idle
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
#define UART_THREAD_WA_SIZE     512U

/* UART bridge buffer size. */
#define UART_BRIDGE_BUF_SIZE    64U

/*===========================================================================*/
/* RAMFUNC: Flash unique ID reading via SSI.                                 */
/*===========================================================================*/

/*
 * All functions that run with XIP disabled must be in RAM.
 * The .ramtext section is copied from flash to RAM by CRT0 at boot.
 */
#define RAMFUNC __attribute__((noinline, section(".ramtext")))

/* SSI register offsets. */
#define SSI_BASE                0x18000000U
#define SSI_CTRLR0_OFF          0x00U
#define SSI_CTRLR1_OFF          0x04U
#define SSI_SSIENR_OFF          0x08U
#define SSI_SER_OFF             0x10U
#define SSI_BAUDR_OFF           0x14U
#define SSI_RXFLR_OFF           0x24U
#define SSI_SR_OFF              0x28U
#define SSI_DR0_OFF             0x60U
#define SSI_SPI_CTRLR0_OFF      0xF4U

#define SSI_SR_BUSY             (1U << 0)
#define SSI_CTRLR0_DFS_8BIT     (7U << 16)
#define SSI_BAUDR_DEFAULT       6U
#define SSI_CTRLR0_XIP          0x001F0300U
#define SSI_SPI_CTRLR0_XIP      0x03000218U

/* IO QSPI registers. */
#define IOQSPI_BASE             0x40018000U
#define IOQSPI_SS_CTRL_OFF      0x0CU

/* PADS QSPI registers. */
#define PADS_QSPI_BASE          0x40020000U
#define PADS_QSPI_SD0_OFF       0x08U
#define PADS_QSPI_SD1_OFF       0x0CU
#define PADS_QSPI_SD2_OFF       0x10U
#define PADS_QSPI_SD3_OFF       0x14U
#define PADS_OD                 (1U << 7)
#define PADS_IE                 (1U << 6)
#define PADS_PUE                (1U << 3)
#define PADS_PDE                (1U << 2)

/* XIP controller. */
#define XIP_CTRL_BASE           0x14000000U
#define XIP_CTRL_OFF            0x00U
#define XIP_FLUSH_OFF           0x04U

#define FLASHCMD_UNIQUE_ID      0x4BU

/**
 * @brief   Force QSPI chip select high or low.
 * @note    RAMFUNC — runs from RAM.
 */
RAMFUNC static void flash_cs_force(bool high) {
  volatile uint32_t *ss_ctrl =
      (volatile uint32_t *)(IOQSPI_BASE + IOQSPI_SS_CTRL_OFF);
  uint32_t val = high ? 3U : 2U;  /* OUTOVER_HIGH=3, OUTOVER_LOW=2 */

  *ss_ctrl = (*ss_ctrl & ~(3U << 8)) | (val << 8);
  (void)*ss_ctrl;  /* Read-back to flush write. */
}

/**
 * @brief   Exit XIP mode and configure SSI for direct SPI access.
 * @note    RAMFUNC — runs from RAM. Pattern from ChibiOS EFL driver.
 */
RAMFUNC static void flash_exit_xip(volatile uint32_t *ssi) {
  volatile uint32_t *pads = (volatile uint32_t *)PADS_QSPI_BASE;
  uint32_t pad_save, pad_tmp;
  unsigned i;
  volatile unsigned delay;

  /* Wait for SSI idle. */
  while ((ssi[SSI_SR_OFF / 4U] & SSI_SR_BUSY) != 0U) {
  }

  /* Configure SSI for standard 8-bit SPI. */
  ssi[SSI_SSIENR_OFF / 4U] = 0U;
  ssi[SSI_BAUDR_OFF / 4U]  = SSI_BAUDR_DEFAULT;
  ssi[SSI_CTRLR0_OFF / 4U] = SSI_CTRLR0_DFS_8BIT;
  ssi[SSI_SER_OFF / 4U]    = 1U;
  ssi[SSI_SSIENR_OFF / 4U] = 1U;

  /* Save pad state, set output-disabled with pull-down. */
  pad_save = pads[PADS_QSPI_SD0_OFF / 4U];
  pad_tmp  = (pad_save & ~(PADS_OD | PADS_PUE | PADS_PDE))
             | PADS_OD | PADS_PDE;

  /* Step 1: CS high, 32 clocks with IO pulled down. */
  flash_cs_force(true);
  pads[PADS_QSPI_SD0_OFF / 4U] = pad_tmp;
  pads[PADS_QSPI_SD1_OFF / 4U] = pad_tmp;
  pads[PADS_QSPI_SD2_OFF / 4U] = pad_tmp;
  pads[PADS_QSPI_SD3_OFF / 4U] = pad_tmp;

  for (delay = 0U; delay < 2048U; delay++) {
  }

  for (i = 0U; i < 4U; i++) {
    ssi[SSI_DR0_OFF / 4U] = 0U;
    while (ssi[SSI_RXFLR_OFF / 4U] == 0U) {
    }
    (void)ssi[SSI_DR0_OFF / 4U];
  }

  /* Step 2: CS low, 32 clocks with IO pulled up. */
  pad_tmp = (pad_tmp & ~PADS_PDE) | PADS_PUE;
  flash_cs_force(false);
  pads[PADS_QSPI_SD0_OFF / 4U] = pad_tmp;
  pads[PADS_QSPI_SD1_OFF / 4U] = pad_tmp;
  pads[PADS_QSPI_SD2_OFF / 4U] = pad_tmp;
  pads[PADS_QSPI_SD3_OFF / 4U] = pad_tmp;

  for (delay = 0U; delay < 2048U; delay++) {
  }

  for (i = 0U; i < 4U; i++) {
    ssi[SSI_DR0_OFF / 4U] = 0U;
    while (ssi[SSI_RXFLR_OFF / 4U] == 0U) {
    }
    (void)ssi[SSI_DR0_OFF / 4U];
  }

  /* Restore pad controls. */
  pads[PADS_QSPI_SD0_OFF / 4U] = pad_save;
  pads[PADS_QSPI_SD1_OFF / 4U] = pad_save;
  pad_save = (pad_save & ~PADS_PDE) | PADS_PUE;
  pads[PADS_QSPI_SD2_OFF / 4U] = pad_save;
  pads[PADS_QSPI_SD3_OFF / 4U] = pad_save;

  /* Step 3: Send 0xFF 0xFF to exit continuous read mode. */
  flash_cs_force(false);
  ssi[SSI_DR0_OFF / 4U] = 0xFFU;
  ssi[SSI_DR0_OFF / 4U] = 0xFFU;
  while (ssi[SSI_RXFLR_OFF / 4U] < 2U) {
  }
  (void)ssi[SSI_DR0_OFF / 4U];
  (void)ssi[SSI_DR0_OFF / 4U];
  flash_cs_force(true);
}

/**
 * @brief   Re-enter XIP mode and flush cache.
 * @note    RAMFUNC — runs from RAM.
 */
RAMFUNC static void flash_enter_xip(volatile uint32_t *ssi) {
  volatile uint32_t *ss_ctrl =
      (volatile uint32_t *)(IOQSPI_BASE + IOQSPI_SS_CTRL_OFF);
  volatile uint32_t *xip = (volatile uint32_t *)XIP_CTRL_BASE;

  /* Reset CS to normal operation. */
  *ss_ctrl = 0U;

  /* Configure SSI for XIP. */
  ssi[SSI_SSIENR_OFF / 4U]     = 0U;
  ssi[SSI_BAUDR_OFF / 4U]      = SSI_BAUDR_DEFAULT;
  ssi[SSI_CTRLR0_OFF / 4U]     = SSI_CTRLR0_XIP;
  ssi[SSI_CTRLR1_OFF / 4U]     = 0U;
  ssi[SSI_SPI_CTRLR0_OFF / 4U] = SSI_SPI_CTRLR0_XIP;
  ssi[SSI_SER_OFF / 4U]        = 1U;
  ssi[SSI_SSIENR_OFF / 4U]     = 1U;

  /* Flush and enable XIP cache. */
  xip[XIP_FLUSH_OFF / 4U] = 1U;
  __DSB();
  __ISB();
  xip[XIP_CTRL_OFF / 4U] = 1U;
}

/**
 * @brief   Read the flash chip's 8-byte unique ID via JEDEC 0x4B command.
 * @note    RAMFUNC — runs from RAM. Exits and re-enters XIP.
 *
 * @param[out] uid  8-byte buffer for the unique ID.
 */
RAMFUNC static void read_flash_unique_id(uint8_t *uid) {
  volatile uint32_t *ssi = (volatile uint32_t *)SSI_BASE;
  unsigned i;

  flash_exit_xip(ssi);

  /* Assert CS. */
  flash_cs_force(false);

  /* Send command byte 0x4B. */
  ssi[SSI_DR0_OFF / 4U] = FLASHCMD_UNIQUE_ID;
  while (ssi[SSI_RXFLR_OFF / 4U] == 0U) {
  }
  (void)ssi[SSI_DR0_OFF / 4U];

  /* Send 4 dummy bytes. */
  for (i = 0U; i < 4U; i++) {
    ssi[SSI_DR0_OFF / 4U] = 0U;
    while (ssi[SSI_RXFLR_OFF / 4U] == 0U) {
    }
    (void)ssi[SSI_DR0_OFF / 4U];
  }

  /* Read 8 bytes of unique ID. */
  for (i = 0U; i < 8U; i++) {
    ssi[SSI_DR0_OFF / 4U] = 0U;
    while (ssi[SSI_RXFLR_OFF / 4U] == 0U) {
    }
    uid[i] = (uint8_t)ssi[SSI_DR0_OFF / 4U];
  }

  /* Deassert CS. */
  flash_cs_force(true);

  flash_enter_xip(ssi);
}

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
/* Shared memory for inter-core communication.                               */
/*===========================================================================*/

static dap_shared_t dap_shared __attribute__((aligned(4)));

/* Handshake flags for boot-time flash ID reading. */
static volatile uint32_t c1_in_ram    = 0U;
static volatile uint32_t init_complete = 0U;

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
  /* Data received on EP1 OUT. */
  dap_rx_len = usbGetReceiveTransactionSizeX(usbp, ep);
  chSysLockFromISR();
  chBSemSignalI(&dap_rx_sem);
  chSysUnlockFromISR();
}

void dap_usb_in_cb(USBDriver *usbp, usbep_t ep) {
  /* IN transfer complete — nothing to do. */
  (void)usbp;
  (void)ep;
}

/*===========================================================================*/
/* DAP Thread (Core 0) — bridges USB ↔ Core 1.                              */
/*===========================================================================*/

static THD_WORKING_AREA(waDapThread, DAP_THREAD_WA_SIZE);
static THD_FUNCTION(DapThread, arg) {
  (void)arg;

  chRegSetThreadName("dap");

  while (true) {
    /* Wait for USB to be configured before arming endpoint. */
    while (serusbcfg.usbp->state != USB_ACTIVE) {
      chThdSleepMilliseconds(10);
    }

    /* Arm EP1 OUT to receive a DAP command. */
    chSysLock();
    usbStartReceiveI(serusbcfg.usbp, DAP_EP, dap_rx_buf, DAP_PACKET_SIZE);
    chSysUnlock();

    /* Wait for USB OUT data. */
    chBSemWait(&dap_rx_sem);

    /* Check for DAP_TransferAbort (0x07) — handle on Core 0. */
    if (dap_rx_buf[0] == DAP_CMD_TRANSFER_ABORT) {
      dap_shared.cmd_buf[0] = DAP_CMD_TRANSFER_ABORT;
      /* Set abort flag for running transfer on Core 1. */
      dap_shared.resp_buf[0] = DAP_CMD_TRANSFER_ABORT;
      dap_shared.resp_buf[1] = DAP_OK;
      dap_shared.resp_len = 2U;
      /* Signal abort to Core 1. */
      __DMB();
      /* Don't go through normal cmd/resp flow — respond immediately. */
      chSysLock();
      usbStartTransmitI(serusbcfg.usbp, DAP_EP, dap_shared.resp_buf,
                          dap_shared.resp_len);
      chSysUnlock();
      continue;
    }

    /* Copy command to shared buffer. */
    memcpy((void *)dap_shared.cmd_buf, dap_rx_buf, dap_rx_len);
    dap_shared.resp_ready = 0U;
    __DSB();
    dap_shared.cmd_ready = 1U;
    __DSB();
    __SEV();  /* Wake Core 1. */

    /* Wait for Core 1 to process command. */
    while (!dap_shared.resp_ready) {
      chThdSleepMicroseconds(10);
    }
    __DSB();

    /* Copy response and send via USB. */
    memcpy(dap_tx_buf, (const void *)dap_shared.resp_buf, dap_shared.resp_len);
    dap_shared.resp_ready = 0U;

    chSysLock();
    usbStartTransmitI(serusbcfg.usbp, DAP_EP, dap_tx_buf,
                        dap_shared.resp_len);
    chSysUnlock();
  }
}

/*===========================================================================*/
/* UART Bridge Thread (Core 0) — bidirectional USB CDC ↔ UART1.             */
/*===========================================================================*/

static THD_WORKING_AREA(waUartThread, UART_THREAD_WA_SIZE);
static THD_FUNCTION(UartThread, arg) {
  (void)arg;

  chRegSetThreadName("uart_bridge");

  uint8_t buf[UART_BRIDGE_BUF_SIZE];
  size_t n;

  while (true) {
    /* USB → UART direction. */
    n = chnReadTimeout(&SDU1, buf, sizeof(buf), TIME_IMMEDIATE);
    if (n > 0U) {
      chnWriteTimeout(&SIOD1, buf, n, TIME_MS2I(10));
    }

    /* UART → USB direction. */
    n = chnReadTimeout(&SIOD1, buf, sizeof(buf), TIME_IMMEDIATE);
    if (n > 0U) {
      chnWriteTimeout(&SDU1, buf, n, TIME_MS2I(10));
    }

    /* Yield if no data in either direction. */
    chThdSleepMicroseconds(100);
  }
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
/* Core 1 — bare-metal DAP command processor.                                */
/*===========================================================================*/

void c1_main(void) {
  static dap_data_t dap_state;

  /* Signal Core 0 that we're about to enter the RAM wait loop. */
  c1_in_ram = 1U;
  __DSB();
  __SEV();

  /* Wait in RAM while Core 0 reads flash unique ID. */
  c1_wait_for_init(&init_complete);

  dap_init(&dap_state);
  dap_state.shared = &dap_shared;

  while (true) {
    /* Wait for command from Core 0. */
    while (!dap_shared.cmd_ready) {
      __WFE();
    }
    __DSB();
    dap_shared.cmd_ready = 0U;

    /* Process the DAP command. */
    dap_shared.resp_len = dap_process_command(&dap_state,
                                               dap_shared.cmd_buf,
                                               dap_shared.resp_buf);
    __DSB();
    dap_shared.resp_ready = 1U;
    __DSB();
    __SEV();  /* Wake Core 0 if waiting. */
  }
}

/*===========================================================================*/
/* Core 0 — main (ChibiOS RT).                                              */
/*===========================================================================*/

int main(void) {
  uint8_t uid[8];
  char serial_hex[17];

  halInit();
  chSysInit();

  /* Initialize shared memory. */
  memset(&dap_shared, 0, sizeof(dap_shared));

  /* Initialize DAP receive semaphore. */
  chBSemObjectInit(&dap_rx_sem, true);

  /*
   * Read flash unique ID for serial number.
   * Core 1 must be safely in RAM before we exit XIP.
   * Core 1 signals c1_in_ram=1 then enters c1_wait_for_init() (RAMFUNC).
   */
  while (!c1_in_ram) {
    /* Spin — Core 1 starts very quickly after chSysInit(). */
  }
  __DSB();

  chSysLock();
  read_flash_unique_id(uid);
  chSysUnlock();

  uid_to_hex(uid, serial_hex, 8U);
  dap_set_serial(serial_hex);
  usb_set_serial_string(serial_hex);

  /* Signal Core 1 to proceed with normal operation. */
  init_complete = 1U;
  __DSB();
  __SEV();

  /* Initialize Serial-over-USB CDC driver (for UART bridge). */
  sduObjectInit(&SDU1);
  sduStart(&SDU1, &serusbcfg);

  /* Start USB with disconnect/reconnect pattern. */
  usbDisconnectBus(serusbcfg.usbp);
  chThdSleepMilliseconds(1500);
  usbStart(serusbcfg.usbp, &usbcfg);
  usbConnectBus(serusbcfg.usbp);

  /* Configure UART1 pins for bridge (GPIO 4 TX, GPIO 5 RX). */
  palSetPadMode(IOPORT1, UART_TX_PIN, PAL_MODE_ALTERNATE_UART);
  palSetPadMode(IOPORT1, UART_RX_PIN, PAL_MODE_ALTERNATE_UART);
  sioStart(&SIOD1, &uart_bridge_config);

  /* Configure LED pin. */
  palSetLineMode(LED_PIN, PAL_MODE_OUTPUT_PUSHPULL);

  /* Start DAP thread (higher priority). */
  chThdCreateStatic(waDapThread, sizeof(waDapThread),
                    NORMALPRIO + 1, DapThread, NULL);

  /* Start UART bridge thread. */
  chThdCreateStatic(waUartThread, sizeof(waUartThread),
                    NORMALPRIO, UartThread, NULL);

  /* Main thread: LED status based on DAP host status. */
  while (true) {
    if (serusbcfg.usbp->state == USB_ACTIVE) {
      if (dap_shared.led_running) {
        /* Debug session active: fast blink. */
        palToggleLine(LED_PIN);
        chThdSleepMilliseconds(100);
      }
      else if (dap_shared.led_connect) {
        /* Connected to debugger, idle: solid ON. */
        palSetLine(LED_PIN);
        chThdSleepMilliseconds(250);
      }
      else {
        /* USB active, no debug session: slow blink. */
        palToggleLine(LED_PIN);
        chThdSleepMilliseconds(500);
      }
    }
    else {
      /* USB not connected: LED off. */
      palClearLine(LED_PIN);
      chThdSleepMilliseconds(250);
    }
  }

  return 0;
}
