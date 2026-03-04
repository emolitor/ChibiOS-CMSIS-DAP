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
 * This code runs on Core 1.
 */

#include "swd.h"
#include "pio_swd.h"

/*===========================================================================*/
/* PIO instance and state machine selection.                                 */
/*===========================================================================*/

#define SWD_PIO         PIO0
#define SWD_SM          0U

/* IO_BANK0 function select: PIO0 = 6 */
#define FUNCSEL_PIO0    6U

/* Program offset in instruction memory (set by pio_swd_init). */
static uint32_t prog_offset;

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
#define FUNCSEL_NULL            31U

/*===========================================================================*/
/* PIO SWD helper functions.                                                 */
/*===========================================================================*/

/**
 * @brief   Write bits via PIO (fire-and-forget).
 */
static inline void probe_write_bits(uint32_t bit_count, uint32_t data) {
  pio_swd_put(SWD_PIO, SWD_SM,
              pio_swd_cmd(bit_count, true, PIO_SWD_CMD_WRITE, prog_offset));
  pio_swd_put(SWD_PIO, SWD_SM, data);
}

/**
 * @brief   Read bits via PIO (blocking).
 */
static inline uint32_t probe_read_bits(uint32_t bit_count) {
  pio_swd_put(SWD_PIO, SWD_SM,
              pio_swd_cmd(bit_count, false, PIO_SWD_CMD_READ, prog_offset));
  uint32_t raw = pio_swd_get(SWD_PIO, SWD_SM);
  if (bit_count < 32U)
    raw >>= (32U - bit_count);
  return raw;
}

/**
 * @brief   Clock with SWDIO as input (hi-Z turnaround clocks).
 */
static inline void probe_hiz_clocks(uint32_t bit_count) {
  pio_swd_put(SWD_PIO, SWD_SM,
              pio_swd_cmd(bit_count, false, PIO_SWD_CMD_TURNAROUND,
                          prog_offset));
  pio_swd_put(SWD_PIO, SWD_SM, 0U);  /* Dummy data (write_cmd does pull). */
}

/*===========================================================================*/
/* Public API.                                                               */
/*===========================================================================*/

/**
 * @brief   Initialize SWD pins and PIO state machine.
 */
void swd_init(uint32_t clk_div) {
  /* SWCLK: PIO-controlled via side-set, fast slew, input enabled. */
  PAD_REG(SWD_PIN_SWCLK) = PAD_IE | PAD_SLEWFAST;
  IO_CTRL(SWD_PIN_SWCLK) = FUNCSEL_PIO0;
  SIO_GPIO_OUT_SET = SWCLK_BIT;
  SIO_GPIO_OE_SET  = SWCLK_BIT;

  /* SWDIO: PIO-controlled via out/in pins, fast slew, pull-up, input. */
  PAD_REG(SWD_PIN_SWDIO) = PAD_IE | PAD_PUE | PAD_SLEWFAST;
  IO_CTRL(SWD_PIN_SWDIO) = FUNCSEL_PIO0;
  SIO_GPIO_OUT_SET = SWDIO_BIT;
  SIO_GPIO_OE_SET  = SWDIO_BIT;

  /* nRESET: open-drain (SIO, not PIO), pull-up, deasserted. */
  PAD_REG(SWD_PIN_NRESET) = PAD_IE | PAD_PUE;
  IO_CTRL(SWD_PIN_NRESET) = FUNCSEL_SIO;
  SIO_GPIO_OUT_CLR = NRESET_BIT;
  SIO_GPIO_OE_CLR  = NRESET_BIT;

  /* Release PIO0 from reset (ChibiOS has no PIO driver). */
  hal_lld_peripheral_unreset(RESETS_ALLREG_PIO0);

  /* Initialize PIO state machine. */
  prog_offset = pio_swd_init(SWD_PIO, SWD_SM, SWD_PIN_SWCLK, SWD_PIN_SWDIO);

  /* Set clock divider. */
  pio_swd_set_clkdiv(SWD_PIO, SWD_SM, clk_div);
}

/**
 * @brief   Update PIO clock divider.
 */
void swd_set_clkdiv(uint32_t clk_div) {
  pio_swd_set_clkdiv(SWD_PIO, SWD_SM, clk_div);
}

/**
 * @brief   Tri-state all SWD pins, disable PIO.
 */
void swd_off(void) {
  pio_swd_deinit(SWD_PIO, SWD_SM);

  SIO_GPIO_OE_CLR = SWCLK_BIT | SWDIO_BIT | NRESET_BIT;
  PAD_REG(SWD_PIN_SWCLK)  = PAD_OD;
  PAD_REG(SWD_PIN_SWDIO)  = PAD_OD;
  PAD_REG(SWD_PIN_NRESET) = PAD_OD;
  IO_CTRL(SWD_PIN_SWCLK)  = FUNCSEL_NULL;
  IO_CTRL(SWD_PIN_SWDIO)  = FUNCSEL_NULL;
  IO_CTRL(SWD_PIN_NRESET) = FUNCSEL_NULL;
}

/**
 * @brief   Perform a full SWD transfer (request + ACK + data).
 *
 * @param[in]     request       SWD request byte
 * @param[in,out] data          pointer to 32-bit data
 * @param[in]     clk_div       PIO clock divider (16.8 fixed-point)
 * @param[in]     idle_cycles   number of idle cycles after transfer
 * @param[in]     turnaround    turnaround clock cycles
 * @param[in]     data_phase    if nonzero, clock data phase on WAIT/FAULT
 * @return        ACK value
 */
uint8_t swd_transfer(uint32_t request, uint32_t *data,
                      uint32_t clk_div, uint32_t idle_cycles,
                      uint32_t turnaround, uint32_t data_phase) {
  uint32_t ack;
  uint32_t val;
  uint32_t parity;
  uint32_t bit;

  (void)clk_div;  /* Clock set once at init/reconfigure, not per-transfer. */

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
 * @param[in] clk_div   PIO clock divider (unused, set at init)
 */
void swj_sequence(uint32_t count, const uint8_t *data, uint32_t clk_div) {
  uint32_t remaining = count;

  (void)clk_div;

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
 * @param[in]  clk_div    PIO clock divider (unused, set at init)
 */
void swd_sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi,
                   uint32_t clk_div) {
  uint32_t count = info & 0x3FU;
  uint32_t remaining;
  uint32_t offset = 0U;

  (void)clk_div;

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
