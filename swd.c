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
 * PIO-based SWD driver for CMSIS-DAP probe.
 *
 * Uses PIO0 SM0 for deterministic SWD timing.
 * Pad configuration uses PADS_BANK0 and IO_BANK0 CMSIS structs.
 * State machine allocation and program loading via ChibiOS PIO LLD.
 * This code runs on Core 1.
 */

#include "swd.h"
#include "pio_swd.h"

/*===========================================================================*/
/* PIO state machine (allocated via PIO LLD).                                */
/*===========================================================================*/

/* Allocated PIO state machine (NULL when disconnected). */
static const rp_pio_sm_t *swd_sm;

/* Program offset in instruction memory (set by pioProgramLoad). */
static int32_t prog_offset;

/* The barrier uses a shared PIO flag routed to this core's PIO vector. */
#define PIO_SWD_BARRIER_IRQ        0U
#define PIO_SWD_BARRIER_MASK       (1U << PIO_SWD_BARRIER_IRQ)
#define PIO_SWD_BARRIER_INTE       PIO_IRQ_SM(PIO_SWD_BARRIER_IRQ)
#define PIO_SWD_BARRIER_TIMEOUT_US 2000000U

/*
 * Logical priority of the PIO vector, ChibiOS convention: zero is the most
 * urgent and the handler must stay at or below the kernel priority because
 * it wakes a thread through an I-class service. Three is the least urgent
 * level available on every supported port and matches what mcuconf.h gives
 * the other peripherals.
 */
#define PIO_SWD_IRQ_PRIORITY       3U

/* Thread waiting on the barrier flag, resumed from the PIO handler. */
static thread_reference_t barrier_trp;

/*===========================================================================*/
/* Pad configuration registers (CMSIS struct access).                        */
/*===========================================================================*/

#define PAD_REG(gpio)           (PADS_BANK0->GPIO[(gpio)])
#define IO_CTRL(gpio)           (IO_BANK0->GPIO[(gpio)].CTRL)

/* Pad bits. */
#define PAD_OD                  (1U << 7)   /* Output disable */
#define PAD_IE                  (1U << 6)   /* Input enable */
#define PAD_PUE                 (1U << 3)   /* Pull-up enable */
#define PAD_PDE                 (1U << 2)   /* Pull-down enable */
#define PAD_SLEWFAST            (1U << 0)   /* Fast slew rate */

/* IO_BANK0 function select: SIO = 5 */
#define FUNCSEL_SIO             5U

/*===========================================================================*/
/* PIO SWD helper functions.                                                 */
/*===========================================================================*/

/**
 * @brief   Write bits via PIO (fire-and-forget).
 */
static inline void probe_write_bits(uint32_t bit_count, uint32_t data) {
  pioSmPut(swd_sm,
           pio_swd_cmd(bit_count, true, PIO_SWD_CMD_WRITE,
                       (uint32_t)prog_offset));
  pioSmPut(swd_sm, data);
}

/**
 * @brief   Read bits via PIO (blocking).
 */
static inline uint32_t probe_read_bits(uint32_t bit_count) {
  pioSmPut(swd_sm,
           pio_swd_cmd(bit_count, false, PIO_SWD_CMD_READ,
                       (uint32_t)prog_offset));
  uint32_t raw = pioSmGet(swd_sm);
  if (bit_count < 32U)
    raw >>= (32U - bit_count);
  return raw;
}

/**
 * @brief   Clock with SWDIO as input (hi-Z turnaround clocks).
 */
static inline void probe_hiz_clocks(uint32_t bit_count) {
  pioSmPut(swd_sm,
           pio_swd_cmd(bit_count, false, PIO_SWD_CMD_TURNAROUND,
                       (uint32_t)prog_offset));
  pioSmPut(swd_sm, 0U);  /* Dummy data (write_cmd does pull). */
}

/**
 * @brief   PIO block handler: acknowledges the barrier flag and wakes the
 *          waiting thread.
 * @note    The LLD acknowledges nothing itself. IRQn_INTS is a live view of
 *          INTR, so the flag has to be cleared here or the handler would be
 *          re-entered forever.
 */
static void probe_barrier_isr(void *param, uint32_t ints) {
  (void)param;

  if ((ints & PIO_SWD_BARRIER_INTE) == 0U)
    return;

  pioIrqClearX(RP_PIO0_BLOCK, PIO_SWD_BARRIER_MASK);

  chSysLockFromISR();
  chThdResumeI(&barrier_trp, MSG_OK);
  chSysUnlockFromISR();
}

/**
 * @brief   Wait until all previously queued PIO commands have completed.
 * @details A dedicated command sets a shared PIO IRQ flag after every word
 *          ahead of it in the TX FIFO has been consumed and executed. The
 *          flag is routed to this core's PIO vector, so the wait suspends
 *          the calling thread instead of spinning on the flag.
 */
static bool probe_barrier(void) {
  sysinterval_t timeout = TIME_US2I(PIO_SWD_BARRIER_TIMEOUT_US);
  systime_t start = chVTGetSystemTimeX();
  systime_t end = chTimeAddX(start, timeout);
  msg_t msg;

  /*
   * Make room for the barrier command without blocking on the FIFO: a
   * wedged program would never drain it and pioSmPut() cannot time out.
   * Normally the FIFO has space and this does not loop at all.
   */
  while (pioSmIsTxFullX(swd_sm)) {
    if (!chVTIsSystemTimeWithinX(start, end))
      return false;
  }

  /*
   * Arming and suspending share one critical section. The handler runs at
   * or below the kernel priority, so it cannot slip in between the two and
   * resume a reference that is not armed yet.
   */
  chSysLock();
  pioIrqClearX(swd_sm->block, PIO_SWD_BARRIER_MASK);
  /* The input direction keeps SWDIO high impedance between transactions. */
  pioSmPutX(swd_sm,
            pio_swd_cmd(1U, false, PIO_SWD_OFFSET_BARRIER_CMD,
                        (uint32_t)prog_offset));
  msg = chThdSuspendTimeoutS(&barrier_trp, timeout);
  chSysUnlock();

  if (msg != MSG_OK) {
    /* Timed out: drop a flag raised after the wait was abandoned. */
    pioIrqClearX(swd_sm->block, PIO_SWD_BARRIER_MASK);
    return false;
  }

  return true;
}

/*===========================================================================*/
/* Public API.                                                               */
/*===========================================================================*/

/**
 * @brief   Initialize SWD pins and PIO state machine.
 */
bool swd_init(uint32_t clk_div) {
  /*
   * Allocate PIO0 SM0 via PIO LLD. No per state machine ISR is needed, the
   * barrier flag is a block resource and is served by a block callback;
   * allocating here is also what enables this core's PIO vector.
   */
  if (swd_sm == NULL) {
    swd_sm = pioSmAlloc(RP_PIO0_BLOCK, 0U, PIO_SWD_IRQ_PRIORITY, NULL, NULL);
    if (swd_sm == NULL)
      return false;

    prog_offset = pioProgramLoad(RP_PIO0_BLOCK, &pio_swd_program);
    if (prog_offset < 0) {
      pioSmFree(swd_sm);
      swd_sm = NULL;
      return false;
    }

    pioIrqClearX(swd_sm->block, PIO_SWD_BARRIER_MASK);

    /*
     * Route the barrier flag to this core. The block is active now that a
     * state machine is allocated; an idle block is held in reset and would
     * swallow the INTE write.
     */
    pioSetBlockCallback(RP_PIO0_BLOCK, probe_barrier_isr, NULL);
    pioEnableInterruptX(RP_PIO0_BLOCK, PIO_SWD_BARRIER_INTE);
  }
  else if (!probe_barrier()) {
    return false;
  }

  /* nRESET: open-drain (SIO, not PIO), pull-up, deasserted. */
  PAD_REG(SWD_PIN_NRESET) = PAD_IE | PAD_PUE;
  IO_CTRL(SWD_PIN_NRESET) = FUNCSEL_SIO;
  SIO_GPIO_OUT_CLR = NRESET_BIT;
  SIO_GPIO_OE_CLR  = NRESET_BIT;

  /* Configure and start state machine. */
  pio_swd_init(swd_sm, (uint32_t)prog_offset, SWD_PIN_SWCLK, SWD_PIN_SWDIO,
               clk_div);

  return true;
}

/**
 * @brief   Update PIO clock divider.
 */
bool swd_set_clkdiv(uint32_t clk_div) {
  if (swd_sm == NULL)
    return false;

  /*
   * Hosts re-send DAP_SWJ_Clock on every connect and often between transfer
   * blocks, usually with the value already in force. An unchanged divider
   * needs neither the barrier round trip nor the register write.
   */
  if (pioSmGetClkdivX(swd_sm) == pio_swd_clkdiv_reg(clk_div))
    return true;

  if (!probe_barrier())
    return false;

  pio_swd_set_clkdiv(swd_sm, clk_div);
  return true;
}

/**
 * @brief   Tri-state all SWD pins, disable PIO.
 */
bool swd_off(void) {
  bool barrier_ok = true;

  if (swd_sm != NULL) {
    barrier_ok = probe_barrier();

    /*
     * Silence the source before the block is released. Freeing the last SM
     * and unloading the last program resets the block, which drops both the
     * INTE routing and the callback registration; doing it explicitly first
     * keeps the handler from running against a freed state machine.
     */
    pioDisableInterruptX(RP_PIO0_BLOCK, PIO_SWD_BARRIER_INTE);
    pioSetBlockCallback(RP_PIO0_BLOCK, NULL, NULL);

    /*
     * Stop and release the SM before erasing its live instruction memory.
     * Current ChibiOS deliberately keeps a PIO block out of reset while
     * either an SM or a program is allocated.
     */
    pioSmFree(swd_sm);
    pioProgramUnload(RP_PIO0_BLOCK, prog_offset, PIO_SWD_PROGRAM_LEN);
    swd_sm = NULL;
  }

  SIO_GPIO_OE_CLR = SWCLK_BIT | SWDIO_BIT | NRESET_BIT;
  PAD_REG(SWD_PIN_SWCLK)  = PAD_OD;
  PAD_REG(SWD_PIN_SWDIO)  = PAD_OD;
  PAD_REG(SWD_PIN_NRESET) = PAD_OD;
  IO_CTRL(SWD_PIN_SWCLK)  = RP_PIO_FUNCSEL_NULL;
  IO_CTRL(SWD_PIN_SWDIO)  = RP_PIO_FUNCSEL_NULL;
  IO_CTRL(SWD_PIN_NRESET) = RP_PIO_FUNCSEL_NULL;

  return barrier_ok;
}

/**
 * @brief   Perform a full SWD transfer (request + ACK + data).
 *
 * @param[in]     request       SWD request byte
 * @param[in,out] data          pointer to 32-bit data
 * @param[in]     idle_cycles   number of idle cycles after transfer
 * @param[in]     turnaround    turnaround clock cycles
 * @param[in]     data_phase    if nonzero, clock data phase on WAIT/FAULT
 * @return        ACK value
 */
RAMFUNC uint8_t swd_transfer(uint32_t request, uint32_t *data,
                              uint32_t idle_cycles, uint32_t turnaround,
                              uint32_t data_phase) {
  uint32_t ack;
  uint32_t val;
  uint32_t parity;
  uint32_t bit;

  /* --- Request phase (8 bits) --- */
  probe_write_bits(8U, request);

  /* --- Turnaround + ACK (read together) --- */
  ack = probe_read_bits(turnaround + 3U);
  ack >>= turnaround;  /* Discard turnaround bits. */
  ack &= 0x07U;

  if (ack == SWD_ACK_OK) {
    /* --- Data phase --- */
    if (request & (1U << 2)) {
      /* Read transfer (RnW=1). */
      val = probe_read_bits(32U);
      bit = probe_read_bits(1U);
      parity = (uint32_t)__builtin_popcount(val) & 1U;

      /* Turnaround back to output. */
      probe_hiz_clocks(turnaround);

      if (parity != bit)
        return SWD_ACK_PARITY_ERR;
      if (data)
        *data = val;
    }
    else {
      /* Write transfer (RnW=0): turnaround then data. */
      probe_hiz_clocks(turnaround);

      val = *data;
      probe_write_bits(32U, val);
      parity = (uint32_t)__builtin_popcount(val) & 1U;
      probe_write_bits(1U, parity);
    }

    /* --- Idle cycles --- */
    if (idle_cycles > 0U)
      probe_write_bits(idle_cycles, 0U);
  }
  else if ((ack == SWD_ACK_WAIT) || (ack == SWD_ACK_FAULT)) {
    if (data_phase && (request & (1U << 2))) {
      /* Read: dummy 33 bits (data + parity). */
      probe_read_bits(32U);
      probe_read_bits(1U);
    }
    /* Turnaround back to output. */
    probe_hiz_clocks(turnaround);
    if (data_phase && !(request & (1U << 2))) {
      /* Write: dummy 33 bits. */
      probe_write_bits(32U, 0U);
      probe_write_bits(1U, 0U);
    }
  }
  else {
    /* Protocol error — read back data phase + turnaround. */
    probe_read_bits(32U);
    probe_read_bits(1U);
    probe_hiz_clocks(turnaround);
  }

  return (uint8_t)ack;
}

/**
 * @brief   Output arbitrary bit sequence on SWDIO with SWCLK (SWJ-DP).
 *
 * @param[in] count     number of bits to output
 * @param[in] data      bit data (LSB first, packed bytes)
 */
RAMFUNC void swj_sequence(uint32_t count, const uint8_t *data) {
  uint32_t remaining = count;

  while (remaining > 0U) {
    uint32_t chunk = remaining;
    if (chunk > 32U)
      chunk = 32U;

    /* Pack up to 32 bits from the byte array. */
    uint32_t word = 0U;
    uint32_t n;
    for (n = 0U; n < chunk; n++) {
      uint32_t src_bit = (count - remaining) + n;
      uint32_t bi = src_bit >> 3;
      uint32_t bp = src_bit & 7U;
      word |= (((uint32_t)data[bi] >> bp) & 1U) << n;
    }

    probe_write_bits(chunk, word);
    remaining -= chunk;
  }
}

/**
 * @brief   SWD sequence: output or capture bits.
 *
 * @param[in]  info       bit[5:0]=count (0 means 64), bit[7]=direction
 * @param[in]  swdo       output data (when direction=0)
 * @param[out] swdi       input data (when direction=1)
 */
RAMFUNC void swd_sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi) {
  uint32_t count = info & 0x3FU;
  uint32_t remaining;
  uint32_t offset = 0U;

  if (count == 0U)
    count = 64U;

  remaining = count;

  if (info & 0x80U) {
    /* Input (capture) mode — read in chunks up to 32 bits. */
    while (remaining > 0U) {
      uint32_t chunk = remaining;
      if (chunk > 32U)
        chunk = 32U;

      uint32_t raw = probe_read_bits(chunk);

      /* Unpack bits into byte array. */
      uint32_t n;
      for (n = 0U; n < chunk; n++) {
        uint32_t dst_bit = offset + n;
        uint32_t bi = dst_bit >> 3;
        uint32_t bp = dst_bit & 7U;
        if (bp == 0U)
          swdi[bi] = 0U;
        swdi[bi] |= (uint8_t)(((raw >> n) & 1U) << bp);
      }

      offset += chunk;
      remaining -= chunk;
    }
  }
  else {
    /* Output mode — write in chunks up to 32 bits. */
    while (remaining > 0U) {
      uint32_t chunk = remaining;
      if (chunk > 32U)
        chunk = 32U;

      /* Pack bits from byte array. */
      uint32_t word = 0U;
      uint32_t n;
      for (n = 0U; n < chunk; n++) {
        uint32_t src_bit = offset + n;
        uint32_t bi = src_bit >> 3;
        uint32_t bp = src_bit & 7U;
        word |= (((uint32_t)swdo[bi] >> bp) & 1U) << n;
      }

      probe_write_bits(chunk, word);
      offset += chunk;
      remaining -= chunk;
    }
  }
}
