/*
 * SWD bit-banging driver for RP2040 CMSIS-DAP probe.
 *
 * All GPIO access uses the SIO block (0xD0000000) for single-cycle I/O.
 * Pad configuration uses PADS_BANK0 and IO_BANK0 registers directly.
 * This code runs on Core 1 (bare-metal, no ChibiOS).
 */

#include "swd.h"

/*===========================================================================*/
/* Pad configuration registers.                                              */
/*===========================================================================*/

#define PADS_BANK0_BASE         0x4001C000U
#define IO_BANK0_BASE           0x40014000U

/* Pad register: offset = 4 + gpio*4 */
#define PAD_REG(gpio)           (*(volatile uint32_t *)(PADS_BANK0_BASE + 4U + (gpio) * 4U))
/* IO control register: offset = gpio*8 + 4 */
#define IO_CTRL(gpio)           (*(volatile uint32_t *)(IO_BANK0_BASE + (gpio) * 8U + 4U))

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
/* Clock delay helper.                                                       */
/*===========================================================================*/

static inline void clock_delay(uint32_t delay) {
  volatile uint32_t count = delay;
  while (count--) {
    __asm__ volatile ("");
  }
}

/*===========================================================================*/
/* Low-level SWD bit operations.                                             */
/*===========================================================================*/

static inline void swdio_out_enable(void) {
  SIO_GPIO_OE_SET = SWDIO_BIT;
}

static inline void swdio_out_disable(void) {
  SIO_GPIO_OE_CLR = SWDIO_BIT;
}

static inline void swdio_set(void) {
  SIO_GPIO_OUT_SET = SWDIO_BIT;
}

static inline void swdio_clr(void) {
  SIO_GPIO_OUT_CLR = SWDIO_BIT;
}

static inline uint32_t swdio_read(void) {
  return (SIO_GPIO_IN >> SWD_PIN_SWDIO) & 1U;
}

static inline void swclk_set(void) {
  SIO_GPIO_OUT_SET = SWCLK_BIT;
}

static inline void swclk_clr(void) {
  SIO_GPIO_OUT_CLR = SWCLK_BIT;
}

static inline void swclk_cycle(uint32_t delay) {
  swclk_clr();
  clock_delay(delay);
  swclk_set();
  clock_delay(delay);
}

/* Write a single bit on SWDIO, clock it. */
static inline void swd_write_bit(uint32_t bit, uint32_t delay) {
  if (bit & 1U)
    swdio_set();
  else
    swdio_clr();
  swclk_clr();
  clock_delay(delay);
  swclk_set();
  clock_delay(delay);
}

/* Read a single bit from SWDIO, clock it. */
static inline uint32_t swd_read_bit(uint32_t delay) {
  uint32_t bit;
  swclk_clr();
  clock_delay(delay);
  bit = swdio_read();
  swclk_set();
  clock_delay(delay);
  return bit;
}

/*===========================================================================*/
/* Public API.                                                               */
/*===========================================================================*/

/**
 * @brief   Initialize SWD pins for debug output.
 */
void swd_init(uint32_t clk_delay) {
  (void)clk_delay;

  /* SWCLK: output, push-pull, fast slew, drive high (idle). */
  PAD_REG(SWD_PIN_SWCLK) = PAD_IE | PAD_SLEWFAST;
  IO_CTRL(SWD_PIN_SWCLK) = FUNCSEL_SIO;
  SIO_GPIO_OUT_SET = SWCLK_BIT;
  SIO_GPIO_OE_SET  = SWCLK_BIT;

  /* SWDIO: output initially, push-pull, fast slew, pull-up, drive high. */
  PAD_REG(SWD_PIN_SWDIO) = PAD_IE | PAD_PUE | PAD_SLEWFAST;
  IO_CTRL(SWD_PIN_SWDIO) = FUNCSEL_SIO;
  SIO_GPIO_OUT_SET = SWDIO_BIT;
  SIO_GPIO_OE_SET  = SWDIO_BIT;

  /* nRESET: open-drain (simulated via OE toggle), pull-up, high (deasserted). */
  PAD_REG(SWD_PIN_NRESET) = PAD_IE | PAD_PUE;
  IO_CTRL(SWD_PIN_NRESET) = FUNCSEL_SIO;
  SIO_GPIO_OUT_CLR = NRESET_BIT;  /* Output value stays low */
  SIO_GPIO_OE_CLR  = NRESET_BIT;  /* OE off = pulled high by pull-up */
}

/**
 * @brief   Tri-state all SWD pins.
 */
void swd_off(void) {
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
 * @param[in]     request       SWD request byte (Start, APnDP, RnW, A2, A3, Parity, Stop, Park)
 * @param[in,out] data          pointer to 32-bit data (read or written)
 * @param[in]     clk_delay     clock delay count
 * @param[in]     idle_cycles   number of idle cycles after transfer
 * @param[in]     turnaround    turnaround clock cycles
 * @param[in]     data_phase    if nonzero, clock data phase on WAIT/FAULT
 * @return        ACK value (SWD_ACK_OK, SWD_ACK_WAIT, SWD_ACK_FAULT, SWD_ACK_PARITY_ERR)
 */
uint8_t swd_transfer(uint32_t request, uint32_t *data,
                      uint32_t clk_delay, uint32_t idle_cycles,
                      uint32_t turnaround, uint32_t data_phase) {
  uint32_t ack = 0;
  uint32_t bit;
  uint32_t val;
  uint32_t parity;
  uint32_t n;

  /* --- Request phase (8 bits, MSB first in wire order) --- */
  swdio_out_enable();
  for (n = 0; n < 8U; n++) {
    swd_write_bit((request >> n) & 1U, clk_delay);
  }

  /* --- Turnaround (SWDIO -> input) --- */
  swdio_out_disable();
  for (n = 0; n < turnaround; n++) {
    swclk_cycle(clk_delay);
  }

  /* --- ACK phase (3 bits) --- */
  for (n = 0; n < 3U; n++) {
    bit = swd_read_bit(clk_delay);
    ack |= bit << n;
  }

  if (ack == SWD_ACK_OK) {
    /* --- Data phase --- */
    if (request & (1U << 2)) {
      /* Read transfer (RnW=1): read 32 data bits + 1 parity bit. */
      val = 0;
      parity = 0;
      for (n = 0; n < 32U; n++) {
        bit = swd_read_bit(clk_delay);
        val |= bit << n;
        parity ^= bit;
      }
      /* Parity bit. */
      bit = swd_read_bit(clk_delay);
      parity ^= bit;

      /* Turnaround (back to output). */
      for (n = 0; n < turnaround; n++) {
        swclk_cycle(clk_delay);
      }
      swdio_out_enable();

      if (parity) {
        /* Parity error. */
        return SWD_ACK_PARITY_ERR;
      }
      if (data)
        *data = val;
    }
    else {
      /* Write transfer (RnW=0): turnaround then write 32 data bits + parity. */
      for (n = 0; n < turnaround; n++) {
        swclk_cycle(clk_delay);
      }
      swdio_out_enable();

      val = *data;
      parity = 0;
      for (n = 0; n < 32U; n++) {
        swd_write_bit((val >> n) & 1U, clk_delay);
        parity ^= (val >> n) & 1U;
      }
      /* Parity bit. */
      swd_write_bit(parity, clk_delay);
    }

    /* --- Idle cycles --- */
    swdio_clr();
    for (n = 0; n < idle_cycles; n++) {
      swclk_cycle(clk_delay);
    }
    swdio_set();
  }
  else if ((ack == SWD_ACK_WAIT) || (ack == SWD_ACK_FAULT)) {
    /* On WAIT/FAULT, optionally clock through data phase per data_phase config. */
    if (data_phase && (request & (1U << 2))) {
      /* Read: dummy clock data + parity (33 bits) before turnaround. */
      for (n = 0; n < 33U; n++) {
        swclk_cycle(clk_delay);
      }
    }
    /* Turnaround back to output. */
    for (n = 0; n < turnaround; n++) {
      swclk_cycle(clk_delay);
    }
    swdio_out_enable();
    if (data_phase && !(request & (1U << 2))) {
      /* Write: drive 33 zero bits (dummy data + parity) after turnaround. */
      swdio_clr();
      for (n = 0; n < 33U; n++) {
        swclk_cycle(clk_delay);
      }
    }
    swdio_set();
  }
  else {
    /* Protocol error — turnaround + data phase + turnaround. */
    for (n = 0; n < 33U + turnaround; n++) {
      swclk_cycle(clk_delay);
    }
    swdio_out_enable();
    swdio_set();
  }

  return (uint8_t)ack;
}

/**
 * @brief   Output arbitrary bit sequence on SWDIO with SWCLK (SWJ-DP).
 *
 * @param[in] count     number of bits to output
 * @param[in] data      bit data (LSB first, packed bytes)
 * @param[in] clk_delay clock delay count
 */
void swj_sequence(uint32_t count, const uint8_t *data, uint32_t clk_delay) {
  uint32_t n;
  uint32_t byte_idx;
  uint32_t bit_idx;

  swdio_out_enable();

  for (n = 0; n < count; n++) {
    byte_idx = n >> 3;
    bit_idx = n & 7U;
    swd_write_bit((data[byte_idx] >> bit_idx) & 1U, clk_delay);
  }
}

/**
 * @brief   SWD sequence: output or capture bits.
 *
 * @param[in]  info       bit[7:0]=count (0 means 64), bit[7]=direction (0=out, 1=in)
 * @param[in]  swdo       output data (when direction=0)
 * @param[out] swdi       input data (when direction=1)
 * @param[in]  clk_delay  clock delay count
 */
void swd_sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi,
                   uint32_t clk_delay) {
  uint32_t count = info & 0x3FU;
  uint32_t n;
  uint32_t byte_idx;
  uint32_t bit_idx;
  uint32_t bit;

  if (count == 0U)
    count = 64U;

  if (info & 0x80U) {
    /* Input (capture) mode. */
    swdio_out_disable();
    for (n = 0; n < count; n++) {
      byte_idx = n >> 3;
      bit_idx = n & 7U;
      if (bit_idx == 0U)
        swdi[byte_idx] = 0U;
      bit = swd_read_bit(clk_delay);
      swdi[byte_idx] |= (uint8_t)(bit << bit_idx);
    }
    swdio_out_enable();
  }
  else {
    /* Output mode. */
    swdio_out_enable();
    for (n = 0; n < count; n++) {
      byte_idx = n >> 3;
      bit_idx = n & 7U;
      swd_write_bit((swdo[byte_idx] >> bit_idx) & 1U, clk_delay);
    }
  }
}
