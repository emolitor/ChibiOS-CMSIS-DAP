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
 * RTT (Real-Time Transfer) host-side protocol handler.
 *
 * Reads RTT data from a debug target via SWD MEM-AP background memory
 * access.  Runs on Core 1 in DapProcessThread context, interleaved
 * between CMSIS-DAP commands.
 *
 * Protocol reference: SEGGER RTT specification.
 * Implementation based on probe-rs (MIT/Apache-2.0) algorithms.
 */

#ifndef RTT_H
#define RTT_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* RTT configuration.                                                        */
/*===========================================================================*/

/* Default target RAM search region. */
#define RTT_SCAN_START          0x20000000U
#define RTT_SCAN_SIZE           0x00040000U  /* 256 KB */

/* Scan chunk: words read per scan iteration. */
#define RTT_SCAN_WORDS          64U  /* 256 bytes */

/* Adaptive polling bounds (milliseconds). */
#define RTT_MIN_POLL_MS         8U
#define RTT_MAX_POLL_MS         64U   /* Must be < host poll interval (~100ms) */

/* Maximum bytes read from ring buffer per poll. */
#define RTT_READ_MAX            256U

/*===========================================================================*/
/* RTT control block layout offsets.                                         */
/*===========================================================================*/

/* Control block header: 16 bytes ID + 4 bytes MaxNumUp + 4 bytes MaxNumDown. */
#define RTT_CB_ID_SIZE          16U
#define RTT_CB_HEADER_SIZE      24U

/* Channel descriptor: 6 × 4 bytes = 24 bytes.
 *   +0x00 sName, +0x04 pBuffer, +0x08 SizeOfBuffer,
 *   +0x0C WrOff, +0x10 RdOff, +0x14 Flags */
#define RTT_CHANNEL_SIZE        24U
#define RTT_CH_WROFF            0x0CU
#define RTT_CH_RDOFF            0x10U

/*===========================================================================*/
/* RTT state.                                                                */
/*===========================================================================*/

typedef struct {
  /* Control block address in target RAM (0 = not found). */
  uint32_t  cb_addr;

  /* Channel counts from control block header. */
  uint32_t  num_up;
  uint32_t  num_down;

  /* Cached up channel 0 descriptor (static fields). */
  uint32_t  up0_buf_addr;
  uint32_t  up0_buf_size;

  /* Cached down channel 0 descriptor (static fields). */
  uint32_t  down0_buf_addr;
  uint32_t  down0_buf_size;

  /* Scan state. */
  uint32_t  scan_addr;
  uint32_t  scan_end;
  bool      scan_complete;

  /* Discovery state. */
  bool      found;

  /* Previous debug port (to detect connect/disconnect). */
  uint8_t   last_port;

  /* Adaptive polling interval. */
  uint32_t  poll_interval_ms;

  /* MEM-AP SELECT value (AP base address). Auto-detected from DPIDR. */
  uint32_t  ap_sel;
  bool      ap_detected;
  bool      adiv6;        /* true = ADIv6 (DPv3+), false = ADIv5 (DPv2) */
} rtt_state_t;

/*===========================================================================*/
/* Function prototypes.                                                      */
/*===========================================================================*/

/**
 * @brief   Initialize RTT state.
 */
void rtt_init(rtt_state_t *rtt);

/**
 * @brief   One RTT poll cycle: scan or read up-channel data.
 *
 * @param[in]  rtt          RTT state
 * @param[in]  debug_port   current SWD port state (0=disconnected, 1=SWD)
 * @param[in]  idle_cycles  SWD idle cycles after writes
 * @param[in]  turnaround   SWD turnaround cycles
 * @param[in]  data_phase   SWD data phase on WAIT/FAULT
 * @param[out] buf          buffer for up-channel data
 * @param[in]  max_len      buffer size
 * @return     bytes of up-channel data read (0 if empty or scanning)
 */
uint32_t rtt_poll(rtt_state_t *rtt, uint8_t debug_port,
                  uint32_t idle_cycles,
                  uint32_t turnaround, uint32_t data_phase,
                  uint8_t *buf, uint32_t max_len);

/**
 * @brief   Write data to RTT down-channel 0 (host → target).
 *
 * @param[in]  rtt          RTT state (must be found)
 * @param[in]  idle_cycles  SWD idle cycles after writes
 * @param[in]  turnaround   SWD turnaround cycles
 * @param[in]  data_phase   SWD data phase on WAIT/FAULT
 * @param[in]  data         data to write
 * @param[in]  len          data length
 */
void rtt_write_down(rtt_state_t *rtt, uint32_t idle_cycles,
                    uint32_t turnaround, uint32_t data_phase,
                    const uint8_t *data, uint32_t len);

#endif /* RTT_H */
