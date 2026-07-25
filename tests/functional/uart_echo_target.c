/*
 * RAM-resident UART0 echo target used by uart_link_test.py.
 *
 * This is deliberately freestanding: OpenOCD loads it at 0x20010000 and
 * starts core 0 there while the opposite board remains the USB probe.
 */

#include <stdint.h>

#ifndef UART_BASE
#error "UART_BASE is required"
#endif
#ifndef IO_BANK0_BASE
#error "IO_BANK0_BASE is required"
#endif
#ifndef PADS_BANK0_BASE
#error "PADS_BANK0_BASE is required"
#endif
#ifndef RESETS_BASE
#error "RESETS_BASE is required"
#endif
#ifndef RESET_UART0
#error "RESET_UART0 is required"
#endif
#ifndef UART_IBRD
#error "UART_IBRD is required"
#endif
#ifndef UART_FBRD
#error "UART_FBRD is required"
#endif
#ifndef UART_LCR_H
#error "UART_LCR_H is required"
#endif

#define REG32(address) (*(volatile uint32_t *)(uintptr_t)(address))

#define UART_DR        REG32(UART_BASE + 0x000U)
#define UART_FR        REG32(UART_BASE + 0x018U)
#define UART_IBRD_REG  REG32(UART_BASE + 0x024U)
#define UART_FBRD_REG  REG32(UART_BASE + 0x028U)
#define UART_LCR_H_REG REG32(UART_BASE + 0x02CU)
#define UART_CR        REG32(UART_BASE + 0x030U)
#define UART_IFLS      REG32(UART_BASE + 0x034U)
#define UART_ICR       REG32(UART_BASE + 0x044U)

#define UART_FR_RXFE   (1U << 4)
#define UART_FR_TXFF   (1U << 5)
#define UART_CR_UARTEN (1U << 0)
#define UART_CR_TXE    (1U << 8)
#define UART_CR_RXE    (1U << 9)

#define RESETS_RESET_DONE REG32(RESETS_BASE + 0x008U)
#define RESETS_CLR_RESET  REG32(RESETS_BASE + 0x3000U)

#define GPIO0_CTRL REG32(IO_BANK0_BASE + 0x004U)
#define GPIO1_CTRL REG32(IO_BANK0_BASE + 0x00CU)
#define GPIO0_PAD  REG32(PADS_BANK0_BASE + 0x004U)
#define GPIO1_PAD  REG32(PADS_BANK0_BASE + 0x008U)

__attribute__((noreturn, used))
void uart_echo_entry(void) {
  RESETS_CLR_RESET = RESET_UART0;
  while ((RESETS_RESET_DONE & RESET_UART0) == 0U) {
  }

  /* UART0 TX on GPIO0 and RX on GPIO1. */
  GPIO0_CTRL = 2U;
  GPIO1_CTRL = 2U;
  GPIO0_PAD = (1U << 6) | (1U << 4);
  GPIO1_PAD = (1U << 6) | (1U << 4);

  UART_CR = 0U;
  UART_IBRD_REG = UART_IBRD;
  UART_FBRD_REG = UART_FBRD;
  UART_LCR_H_REG = UART_LCR_H;
  UART_IFLS = 0U;
  UART_ICR = 0x7FFU;
  UART_CR = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;

  while (1) {
    if ((UART_FR & UART_FR_RXFE) == 0U) {
      uint32_t value = UART_DR;
      while ((UART_FR & UART_FR_TXFF) != 0U) {
      }
      UART_DR = value;
    }
  }
}
