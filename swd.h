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
 * SWD bit-banging driver for RP2040 CMSIS-DAP probe.
 * Runs on Core 1 using direct SIO register access.
 */

#ifndef SWD_H
#define SWD_H

#include <stdint.h>

/*===========================================================================*/
/* Pin definitions (matching Pico Debug Probe layout).                       */
/*===========================================================================*/

#define SWD_PIN_NRESET          1U
#define SWD_PIN_SWCLK           2U
#define SWD_PIN_SWDIO           3U

/*===========================================================================*/
/* SIO register addresses for single-cycle GPIO access.                      */
/*===========================================================================*/

#define SIO_BASE                0xD0000000U
#define SIO_GPIO_IN             (*(volatile uint32_t *)(SIO_BASE + 0x004U))
#define SIO_GPIO_OUT_SET        (*(volatile uint32_t *)(SIO_BASE + 0x014U))
#define SIO_GPIO_OUT_CLR        (*(volatile uint32_t *)(SIO_BASE + 0x018U))
#define SIO_GPIO_OE_SET         (*(volatile uint32_t *)(SIO_BASE + 0x024U))
#define SIO_GPIO_OE_CLR         (*(volatile uint32_t *)(SIO_BASE + 0x028U))

#define SWCLK_BIT               (1U << SWD_PIN_SWCLK)
#define SWDIO_BIT               (1U << SWD_PIN_SWDIO)
#define NRESET_BIT              (1U << SWD_PIN_NRESET)

/*===========================================================================*/
/* SWD transfer response codes.                                              */
/*===========================================================================*/

#define SWD_ACK_OK              0x01U
#define SWD_ACK_WAIT            0x02U
#define SWD_ACK_FAULT           0x04U
#define SWD_ACK_PARITY_ERR      0x08U

/*===========================================================================*/
/* Function prototypes.                                                      */
/*===========================================================================*/

void swd_init(uint32_t clock_delay);
void swd_off(void);
uint8_t swd_transfer(uint32_t request, uint32_t *data,
                      uint32_t clock_delay, uint32_t idle_cycles,
                      uint32_t turnaround, uint32_t data_phase);
void swj_sequence(uint32_t count, const uint8_t *data, uint32_t clock_delay);
void swd_sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi,
                   uint32_t clock_delay);

#endif /* SWD_H */
