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
 * Reads the SEGGER RTT control block and ring buffers from a debug
 * target via SWD MEM-AP background memory access.  All functions run
 * on Core 1 in DapProcessThread context.
 *
 * MEM-AP access uses the AHB-AP: CSW/TAR/DRW registers.
 * ADIv5 (RP2040): AP 0, bank 0.  ADIv6 (RP2350): AP 0x2000, bank 0xD00.
 * TAR auto-increment wraps at 1 KB boundaries.
 *
 * Protocol reference: SEGGER RTT specification.
 * Algorithms derived from probe-rs (MIT/Apache-2.0).
 */

#include <string.h>
#include "ch.h"
#include "hal.h"
#include "rtt.h"
#include "swd.h"

/*===========================================================================*/
/* Precomputed SWD request bytes for MEM-AP access.                          */
/*===========================================================================*/

/*
 * SWD request byte format (8 bits):
 *   bit 0: Start (1)
 *   bit 1: APnDP
 *   bit 2: RnW
 *   bit 3: A[2]
 *   bit 4: A[3]
 *   bit 5: Parity (XOR of bits 1–4)
 *   bit 6: Stop (0)
 *   bit 7: Park (1)
 */
#define SWD_REQ_DP_WR_ABORT   0x81U  /* DP Write A[3:2]=00 (ABORT)   */
#define SWD_REQ_DP_WR_SELECT  0xB1U  /* DP Write A[3:2]=10 (SELECT)  */
#define SWD_REQ_AP_WR_CSW     0xA3U  /* AP Write A[3:2]=00 (CSW)     */
#define SWD_REQ_AP_RD_CSW     0x87U  /* AP Read  A[3:2]=00 (CSW)     */
#define SWD_REQ_AP_WR_TAR     0x8BU  /* AP Write A[3:2]=01 (TAR)     */
#define SWD_REQ_AP_RD_DRW     0x9FU  /* AP Read  A[3:2]=11 (DRW)     */
#define SWD_REQ_AP_WR_DRW     0xBBU  /* AP Write A[3:2]=11 (DRW)     */
#define SWD_REQ_DP_RD_RDBUFF  0xBDU  /* DP Read  A[3:2]=11 (RDBUFF)  */

/* AHB5-AP CSW: DbgSwEnable + MasterType(debug) + Privileged + word + incr.
 * Bit 31: DbgSwEnable (required for bus transactions)
 * Bit 30: HNONSEC=0 (secure access — RP2350 boots secure)
 * Bit 29: MasterType=1 (debug requester ID)
 * Bit 25: HPROT[1]=1 (privileged)
 * Bits [5:4]=01 (auto-increment single)
 * Bits [2:0]=010 (32-bit) */
#define AHB_AP_CSW_VALUE      0xA2000012U

/* ADIv6 MEM-AP register bank offset.
 * On ADIv6, CSW/TAR/DRW are at AP_base + 0xD00, not AP_base + 0x000. */
#define ADIV6_MEM_AP_BANK     0xD00U

/* Retry count for SWD WAIT responses during RTT operations. */
#define RTT_SWD_RETRIES       3U

/*===========================================================================*/
/* RTT control block magic (little-endian 32-bit words).                     */
/*===========================================================================*/

/* "SEGGER RTT\0\0\0\0\0\0" as four 32-bit LE words. */
#define RTT_MAGIC_W0          0x47474553U  /* "SEGG" */
#define RTT_MAGIC_W1          0x52205245U  /* "ER R" */
#define RTT_MAGIC_W2          0x00005454U  /* "TT\0\0" */
#define RTT_MAGIC_W3          0x00000000U  /* "\0\0\0\0" */

/*===========================================================================*/
/* Static buffers (single-thread access from DapProcessThread).              */
/*===========================================================================*/

static uint32_t scan_buf[RTT_SCAN_WORDS];
static uint32_t read_word_buf[RTT_READ_MAX / 4U];

/*===========================================================================*/
/* SWD helper — single transfer with WAIT retry.                             */
/*===========================================================================*/

/* Module-level idle cycles, set from rtt_poll()/rtt_write_down() context. */
static uint32_t rtt_idle_cycles;

static uint8_t rtt_swd(uint32_t req, uint32_t *data,
                        uint32_t turnaround, uint32_t data_phase) {
  uint32_t retry;
  uint8_t ack;

  for (retry = 0U; retry <= RTT_SWD_RETRIES; retry++) {
    ack = swd_transfer(req, data, 0U, rtt_idle_cycles, turnaround, data_phase);
    if (ack != SWD_ACK_WAIT)
      break;
  }
  return ack;
}

/*===========================================================================*/
/* MEM-AP setup (once per poll cycle).                                       */
/*===========================================================================*/

static bool rtt_mem_setup(uint32_t ap_sel, bool adiv6,
                          uint32_t turnaround, uint32_t data_phase) {
  uint32_t select_val;
  uint32_t csw_val;

  /* ADIv6: CSW/TAR/DRW are in the 0xD00 bank of the AP.
   * ADIv5: CSW/TAR/DRW are in bank 0 (APSEL in bits [31:24]). */
  select_val = adiv6 ? (ap_sel + ADIV6_MEM_AP_BANK) : ap_sel;
  if (rtt_swd(SWD_REQ_DP_WR_SELECT, &select_val,
              turnaround, data_phase) != SWD_ACK_OK)
    return false;

  csw_val = AHB_AP_CSW_VALUE;
  if (rtt_swd(SWD_REQ_AP_WR_CSW, &csw_val,
              turnaround, data_phase) != SWD_ACK_OK)
    return false;

  return true;
}

/*===========================================================================*/
/* MEM-AP word read (handles 1 KB TAR auto-increment boundary).              */
/*===========================================================================*/

static bool rtt_mem_read_words(uint32_t addr, uint32_t *buf, uint32_t count,
                                uint32_t turnaround, uint32_t data_phase) {
  while (count > 0U) {
    /* Words remaining in current 1 KB region. */
    uint32_t region_words = (1024U - (addr & 0x3FFU)) / 4U;
    uint32_t chunk = count < region_words ? count : region_words;
    uint32_t dummy;
    uint32_t i;

    /* Write TAR. */
    if (rtt_swd(SWD_REQ_AP_WR_TAR, &addr, turnaround, data_phase) != SWD_ACK_OK)
      return false;

    /* Prime AP read pipeline (first read is posted, returns stale data). */
    if (rtt_swd(SWD_REQ_AP_RD_DRW, &dummy, turnaround, data_phase) != SWD_ACK_OK)
      return false;

    /* Pipelined reads: each returns the previous read's data. */
    for (i = 0U; i + 1U < chunk; i++) {
      if (rtt_swd(SWD_REQ_AP_RD_DRW, &buf[i], turnaround, data_phase) != SWD_ACK_OK)
        return false;
    }

    /* Flush last word via RDBUFF. */
    if (rtt_swd(SWD_REQ_DP_RD_RDBUFF, &buf[chunk - 1U], turnaround, data_phase) != SWD_ACK_OK)
      return false;

    addr  += chunk * 4U;
    buf   += chunk;
    count -= chunk;
  }
  return true;
}

/*===========================================================================*/
/* MEM-AP byte read (handles unaligned addresses via word reads).            */
/*===========================================================================*/

static bool rtt_mem_read_bytes(uint32_t addr, uint8_t *buf, uint32_t byte_count,
                                uint32_t turnaround, uint32_t data_phase) {
  uint32_t aligned = addr & ~3U;
  uint32_t offset  = addr & 3U;
  uint32_t words   = (offset + byte_count + 3U) / 4U;

  if (words > (RTT_READ_MAX / 4U))
    return false;

  if (!rtt_mem_read_words(aligned, read_word_buf, words, turnaround, data_phase))
    return false;

  memcpy(buf, (uint8_t *)read_word_buf + offset, byte_count);
  return true;
}

/*===========================================================================*/
/* MEM-AP single word write.                                                 */
/*===========================================================================*/

static bool rtt_mem_write_word(uint32_t addr, uint32_t value,
                                uint32_t turnaround, uint32_t data_phase) {
  uint32_t dummy;

  if (rtt_swd(SWD_REQ_AP_WR_TAR, &addr, turnaround, data_phase) != SWD_ACK_OK)
    return false;
  if (rtt_swd(SWD_REQ_AP_WR_DRW, &value, turnaround, data_phase) != SWD_ACK_OK)
    return false;
  /* Verify write completed without fault. */
  if (rtt_swd(SWD_REQ_DP_RD_RDBUFF, &dummy, turnaround, data_phase) != SWD_ACK_OK)
    return false;
  return true;
}

/*===========================================================================*/
/* MEM-AP multi-byte write (word-aligned writes covering byte range).        */
/*===========================================================================*/

static bool rtt_mem_write_bytes(uint32_t addr, const uint8_t *data,
                                 uint32_t byte_count,
                                 uint32_t turnaround, uint32_t data_phase) {
  /* Write word-by-word.  For simplicity, read-modify-write at boundaries
   * if the start/end address is not word-aligned. */
  while (byte_count > 0U) {
    uint32_t word_addr = addr & ~3U;
    uint32_t offset    = addr & 3U;
    uint32_t avail     = 4U - offset;
    uint32_t chunk     = byte_count < avail ? byte_count : avail;
    uint32_t word;

    if (offset != 0U || chunk < 4U) {
      /* Partial word: read-modify-write. */
      if (!rtt_mem_read_words(word_addr, &word, 1U, turnaround, data_phase))
        return false;
      memcpy((uint8_t *)&word + offset, data, chunk);
    }
    else {
      memcpy(&word, data, 4U);
    }

    if (!rtt_mem_write_word(word_addr, word, turnaround, data_phase))
      return false;

    addr       += chunk;
    data       += chunk;
    byte_count -= chunk;
  }
  return true;
}

/*===========================================================================*/
/* Control block discovery.                                                  */
/*===========================================================================*/

/**
 * @brief   Read and cache control block header + channel 0 descriptors.
 */
static bool rtt_read_cb(rtt_state_t *rtt,
                         uint32_t turnaround, uint32_t data_phase) {
  uint32_t header[2];

  /* Read MaxNumUpBuffers and MaxNumDownBuffers. */
  if (!rtt_mem_read_words(rtt->cb_addr + RTT_CB_ID_SIZE, header, 2U,
                           turnaround, data_phase))
    return false;

  rtt->num_up   = header[0];
  rtt->num_down = header[1];

  /* Sanity check. */
  if (rtt->num_up == 0U || rtt->num_up > 16U || rtt->num_down > 16U)
    return false;

  /* Read up channel 0 descriptor (6 words). */
  uint32_t ch0[6];
  if (!rtt_mem_read_words(rtt->cb_addr + RTT_CB_HEADER_SIZE, ch0, 6U,
                           turnaround, data_phase))
    return false;

  rtt->up0_buf_addr = ch0[1];
  rtt->up0_buf_size = ch0[2];

  /* Buffer must be in RAM and non-zero. */
  if (rtt->up0_buf_addr < 0x20000000U || rtt->up0_buf_size == 0U)
    return false;

  /* Read down channel 0 descriptor if available. */
  if (rtt->num_down > 0U) {
    uint32_t down_off = RTT_CB_HEADER_SIZE + rtt->num_up * RTT_CHANNEL_SIZE;
    uint32_t dch0[6];
    if (rtt_mem_read_words(rtt->cb_addr + down_off, dch0, 6U,
                            turnaround, data_phase)) {
      rtt->down0_buf_addr = dch0[1];
      rtt->down0_buf_size = dch0[2];
      if (rtt->down0_buf_addr < 0x20000000U || rtt->down0_buf_size == 0U) {
        rtt->down0_buf_addr = 0U;
        rtt->down0_buf_size = 0U;
      }
    }
  }

  rtt->found = true;
  return true;
}

/**
 * @brief   Scan one chunk of target RAM for the RTT control block.
 * @return  true if the control block was found.
 */
static bool rtt_scan_chunk(rtt_state_t *rtt,
                            uint32_t turnaround, uint32_t data_phase) {
  uint32_t end = rtt->scan_addr + RTT_SCAN_WORDS * 4U;
  uint32_t count;
  uint32_t i;

  if (end > rtt->scan_end)
    end = rtt->scan_end;
  count = (end - rtt->scan_addr) / 4U;
  if (count == 0U) {
    rtt->scan_complete = true;
    return false;
  }

  if (!rtt_mem_read_words(rtt->scan_addr, scan_buf, count,
                           turnaround, data_phase)) {
    rtt->scan_addr = end;
    return false;
  }
  /* Search for the first magic word, then verify the full 16-byte ID. */
  for (i = 0U; i < count; i++) {
    if (scan_buf[i] != RTT_MAGIC_W0)
      continue;

    if (i + 3U < count) {
      /* All four magic words are in this chunk. */
      if (scan_buf[i + 1U] == RTT_MAGIC_W1 &&
          scan_buf[i + 2U] == RTT_MAGIC_W2 &&
          scan_buf[i + 3U] == RTT_MAGIC_W3) {
        rtt->cb_addr = rtt->scan_addr + i * 4U;
        return rtt_read_cb(rtt, turnaround, data_phase);
      }
    }
    else {
      /* Magic spans chunk boundary — re-read from target. */
      uint32_t magic[4];
      uint32_t magic_addr = rtt->scan_addr + i * 4U;
      if (rtt_mem_read_words(magic_addr, magic, 4U,
                              turnaround, data_phase)) {
        if (magic[0] == RTT_MAGIC_W0 && magic[1] == RTT_MAGIC_W1 &&
            magic[2] == RTT_MAGIC_W2 && magic[3] == RTT_MAGIC_W3) {
          rtt->cb_addr = magic_addr;
          return rtt_read_cb(rtt, turnaround, data_phase);
        }
      }
    }
  }

  rtt->scan_addr = end;
  if (rtt->scan_addr >= rtt->scan_end)
    rtt->scan_complete = true;
  return false;
}

/*===========================================================================*/
/* Up channel 0 read (target → host).                                        */
/*===========================================================================*/

static uint32_t rtt_read_up0(rtt_state_t *rtt,
                              uint32_t turnaround, uint32_t data_phase,
                              uint8_t *buf, uint32_t max_len) {
  /* Read WrOff and RdOff from channel 0 descriptor. */
  uint32_t offsets[2];
  uint32_t wroff_addr = rtt->cb_addr + RTT_CB_HEADER_SIZE + RTT_CH_WROFF;

  if (!rtt_mem_read_words(wroff_addr, offsets, 2U, turnaround, data_phase))
    return 0U;

  uint32_t wr = offsets[0];
  uint32_t rd = offsets[1];

  if (wr == rd)
    return 0U;

  /* Sanity. */
  if (wr >= rtt->up0_buf_size || rd >= rtt->up0_buf_size)
    return 0U;

  /* Calculate available bytes. */
  uint32_t available;
  if (wr > rd)
    available = wr - rd;
  else
    available = rtt->up0_buf_size - rd + wr;

  if (available > max_len)
    available = max_len;
  if (available > RTT_READ_MAX)
    available = RTT_READ_MAX;

  /* Read ring buffer data (handle wrap-around). */
  uint32_t first;
  if (wr > rd)
    first = wr - rd;
  else
    first = rtt->up0_buf_size - rd;
  if (first > available)
    first = available;

  if (!rtt_mem_read_bytes(rtt->up0_buf_addr + rd, buf, first,
                           turnaround, data_phase))
    return 0U;

  uint32_t second = available - first;
  if (second > 0U) {
    if (!rtt_mem_read_bytes(rtt->up0_buf_addr, buf + first, second,
                             turnaround, data_phase))
      return 0U;
  }

  /* Update RdOff in target. */
  uint32_t new_rd = (rd + available) % rtt->up0_buf_size;
  uint32_t rdoff_addr = rtt->cb_addr + RTT_CB_HEADER_SIZE + RTT_CH_RDOFF;
  rtt_mem_write_word(rdoff_addr, new_rd, turnaround, data_phase);

  return available;
}

/*===========================================================================*/
/* Public API.                                                               */
/*===========================================================================*/

void rtt_init(rtt_state_t *rtt) {
  memset(rtt, 0, sizeof(rtt_state_t));
  rtt->scan_addr       = RTT_SCAN_START;
  rtt->scan_end        = RTT_SCAN_START + RTT_SCAN_SIZE;
  rtt->poll_interval_ms = RTT_MIN_POLL_MS;
}

uint32_t rtt_poll(rtt_state_t *rtt, uint8_t debug_port,
                  uint32_t idle_cycles,
                  uint32_t turnaround, uint32_t data_phase,
                  uint8_t *buf, uint32_t max_len) {
  uint32_t result = 0U;

  rtt_idle_cycles = idle_cycles;

  /* Reset on connection change. */
  if (debug_port != rtt->last_port) {
    if (rtt->last_port != 0U || debug_port == 0U) {
      /* Port changed — reset RTT state for new target. */
      rtt_init(rtt);
    }
    rtt->last_port = debug_port;
  }

  /* Must be connected via SWD. */
  if (debug_port != 1U)
    return 0U;

  /* Clear any sticky errors before RTT operations.  STICKYERR causes
   * all subsequent SWD transactions to return FAULT until cleared. */
  uint32_t abort_val = 0x1EU;  /* STKERRCLR | WDERRCLR | STKCMPCLR | ORUNERRCLR */
  rtt_swd(SWD_REQ_DP_WR_ABORT, &abort_val, turnaround, data_phase);

  /* Auto-detect ADIv5 vs ADIv6 by probing MEM-AP access.
   * Try ADIv6 first (RP2350: AP 0x2000, bank 0xD00).  If the CSW
   * write gets FAULT, clear errors and try ADIv5 (RP2040: AP 0, bank 0).
   * This avoids relying on DPIDR reads which return stale data from
   * the RTT polling context. */
  if (!rtt->ap_detected) {
    /* Try ADIv6: SELECT = AP_base(0x2000) + bank(0xD00) = 0x2D00. */
    uint32_t select_v6 = 0x00002000U + ADIV6_MEM_AP_BANK;
    uint32_t csw_val   = AHB_AP_CSW_VALUE;
    if (rtt_swd(SWD_REQ_DP_WR_SELECT, &select_v6,
                turnaround, data_phase) == SWD_ACK_OK &&
        rtt_swd(SWD_REQ_AP_WR_CSW, &csw_val,
                turnaround, data_phase) == SWD_ACK_OK) {
      rtt->ap_sel = 0x00002000U;
      rtt->adiv6  = true;
      rtt->ap_detected = true;
    }
    else {
      /* ADIv6 failed — clear sticky errors and try ADIv5. */
      uint32_t abort_v = 0x1EU;
      rtt_swd(SWD_REQ_DP_WR_ABORT, &abort_v, turnaround, data_phase);

      uint32_t select_v5 = 0x00000000U;
      csw_val = AHB_AP_CSW_VALUE;
      if (rtt_swd(SWD_REQ_DP_WR_SELECT, &select_v5,
                  turnaround, data_phase) == SWD_ACK_OK &&
          rtt_swd(SWD_REQ_AP_WR_CSW, &csw_val,
                  turnaround, data_phase) == SWD_ACK_OK) {
        rtt->ap_sel = 0x00000000U;
        rtt->adiv6  = false;
        rtt->ap_detected = true;
      }
      else {
        /* Both failed — clear errors and retry next poll. */
        rtt_swd(SWD_REQ_DP_WR_ABORT, &abort_v, turnaround, data_phase);
        return 0U;
      }
    }
  }

  /* Set up MEM-AP registers (SELECT + CSW).
   * ADIv6: SELECT = ap_sel + 0xD00 = 0x2D00 (RP2350).
   * ADIv5: SELECT = 0x00000000 (RP2040, AP0 bank 0).
   * We do NOT save/restore SELECT.  Both RTT and the host use the
   * same SELECT value for MEM-AP CSW/TAR/DRW access on the primary AP,
   * so leaving it after RTT is safe.  If the host needs a different
   * bank, it will re-write SELECT. */
  if (!rtt_mem_setup(rtt->ap_sel, rtt->adiv6, turnaround, data_phase))
    return 0U;

  /* Scan if not yet found. */
  if (!rtt->found) {
    if (rtt->scan_complete) {
      /* Scan completed without finding CB — restart from the beginning.
       * The target may have been halted during the scan (RTT CB not yet
       * initialized).  Re-scanning will find it once the target is running. */
      rtt->scan_addr     = RTT_SCAN_START;
      rtt->scan_complete = false;
    }
    rtt_scan_chunk(rtt, turnaround, data_phase);
    return 0U;
  }

  /* Read up-channel 0. */
  result = rtt_read_up0(rtt, turnaround, data_phase, buf, max_len);

  return result;
}

void rtt_write_down(rtt_state_t *rtt, uint32_t idle_cycles,
                    uint32_t turnaround, uint32_t data_phase,
                    const uint8_t *data, uint32_t len) {
  rtt_idle_cycles = idle_cycles;
  if (!rtt->found || rtt->down0_buf_size == 0U || len == 0U)
    return;

  /* Clear any sticky errors (same as rtt_poll). */
  uint32_t abort_val = 0x1EU;
  rtt_swd(SWD_REQ_DP_WR_ABORT, &abort_val, turnaround, data_phase);

  /* Read WrOff and RdOff from down channel 0 descriptor. */
  uint32_t down_ch_addr = rtt->cb_addr + RTT_CB_HEADER_SIZE +
                          rtt->num_up * RTT_CHANNEL_SIZE;
  uint32_t offsets[2];
  uint32_t wroff_addr = down_ch_addr + RTT_CH_WROFF;

  /* Set up MEM-AP (SELECT + CSW) — no save/restore, same as rtt_poll(). */
  if (!rtt_mem_setup(rtt->ap_sel, rtt->adiv6, turnaround, data_phase))
    return;

  if (!rtt_mem_read_words(wroff_addr, offsets, 2U, turnaround, data_phase))
    return;

  uint32_t wr = offsets[0];
  uint32_t rd = offsets[1];

  /* Sanity. */
  if (wr >= rtt->down0_buf_size || rd >= rtt->down0_buf_size)
    return;

  /* Calculate free space (leave 1 byte gap). */
  uint32_t free_space;
  if (rd > wr)
    free_space = rd - wr - 1U;
  else
    free_space = rtt->down0_buf_size - wr + rd - 1U;

  if (len > free_space)
    len = free_space;
  if (len == 0U)
    return;

  /* Write data to ring buffer (handle wrap-around). */
  uint32_t first = rtt->down0_buf_size - wr;
  if (first > len)
    first = len;

  rtt_mem_write_bytes(rtt->down0_buf_addr + wr, data, first,
                       turnaround, data_phase);

  uint32_t second = len - first;
  if (second > 0U) {
    rtt_mem_write_bytes(rtt->down0_buf_addr, data + first, second,
                         turnaround, data_phase);
  }

  /* Update WrOff in target. */
  uint32_t new_wr = (wr + len) % rtt->down0_buf_size;
  rtt_mem_write_word(wroff_addr, new_wr, turnaround, data_phase);
}
