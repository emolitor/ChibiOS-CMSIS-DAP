#ifndef TEST_HAL_H
#define TEST_HAL_H

#include <stdint.h>

#include "ch.h"

#define RP_CLK_SYS_FREQ 200000000U

typedef struct {
  uint32_t GPIO_IN;
  uint32_t GPIO_HI_IN;
  uint32_t reserved0[2];
  uint32_t GPIO_OUT;
  uint32_t GPIO_OUT_SET;
  uint32_t GPIO_OUT_CLR;
  uint32_t GPIO_OUT_XOR;
  uint32_t GPIO_OE;
  uint32_t GPIO_OE_SET;
  uint32_t GPIO_OE_CLR;
  uint32_t GPIO_OE_XOR;
} test_sio_t;

extern test_sio_t test_sio;

#define SIO (&test_sio)

#endif /* TEST_HAL_H */
