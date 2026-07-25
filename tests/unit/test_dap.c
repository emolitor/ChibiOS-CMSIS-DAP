#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dap.h"
#include "swd.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

uint32_t test_event_flags;
uint32_t test_delay_us;
rtcnt_t test_realtime_counter;
test_sio_t test_sio;

typedef struct {
  uint32_t request;
  uint32_t data;
  bool     has_data;
} transfer_record_t;

static transfer_record_t transfers[512];
static size_t transfer_count;
static uint8_t ack_script[512];
static uint32_t data_script[512];
static size_t script_count;
static size_t script_index;
static bool swd_init_ok = true;
static uint32_t swd_init_div;
static uint32_t swd_set_div;
static unsigned swd_off_count;
static uint32_t swj_count;
static uint8_t swj_data[32];
static uint32_t sequence_info;
static uint8_t sequence_output[8];
static uint8_t sequence_input[8];

static void fail(const char *expr, const char *file, int line) {
  fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expr);
  exit(1);
}

#define CHECK(expr) do { if (!(expr)) fail(#expr, __FILE__, __LINE__); } while (0)

static void reset_mocks(void) {
  memset(&test_sio, 0, sizeof(test_sio));
  memset(transfers, 0, sizeof(transfers));
  memset(ack_script, SWD_ACK_OK, sizeof(ack_script));
  memset(data_script, 0, sizeof(data_script));
  memset(swj_data, 0, sizeof(swj_data));
  memset(sequence_output, 0, sizeof(sequence_output));
  memset(sequence_input, 0, sizeof(sequence_input));
  transfer_count = 0U;
  script_count = 0U;
  script_index = 0U;
  swd_init_ok = true;
  swd_init_div = 0U;
  swd_set_div = 0U;
  swd_off_count = 0U;
  swj_count = 0U;
  sequence_info = 0U;
  test_event_flags = 0U;
  test_delay_us = 0U;
  test_realtime_counter = 0U;
}

static void script_transfer(uint8_t ack, uint32_t data) {
  CHECK(script_count < ARRAY_LEN(ack_script));
  ack_script[script_count] = ack;
  data_script[script_count] = data;
  script_count++;
}

bool swd_init(uint32_t clk_div) {
  swd_init_div = clk_div;
  return swd_init_ok;
}

void swd_set_clkdiv(uint32_t clk_div) {
  swd_set_div = clk_div;
}

void swd_off(void) {
  swd_off_count++;
}

uint8_t swd_transfer(uint32_t request, uint32_t *data,
                     uint32_t idle_cycles, uint32_t turnaround,
                     uint32_t data_phase) {
  uint8_t ack = SWD_ACK_OK;
  uint32_t scripted_data = 0U;

  (void)idle_cycles;
  (void)turnaround;
  (void)data_phase;
  CHECK(transfer_count < ARRAY_LEN(transfers));

  if (script_index < script_count) {
    ack = ack_script[script_index];
    scripted_data = data_script[script_index];
  }

  transfers[transfer_count].request = request;
  transfers[transfer_count].has_data = data != NULL;
  transfers[transfer_count].data = data != NULL ? *data : 0U;
  transfer_count++;
  script_index++;

  if ((data != NULL) && ((request & (1U << 2)) != 0U))
    *data = scripted_data;

  return ack;
}

void swj_sequence(uint32_t count, const uint8_t *data) {
  size_t bytes = (count + 7U) / 8U;

  CHECK(bytes <= sizeof(swj_data));
  swj_count = count;
  memcpy(swj_data, data, bytes);
}

void swd_sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi) {
  size_t bytes;
  uint32_t count = info & 0x3FU;

  if (count == 0U)
    count = 64U;
  bytes = (count + 7U) / 8U;
  CHECK(bytes <= sizeof(sequence_output));
  sequence_info = info;
  if (swdo != NULL)
    memcpy(sequence_output, swdo, bytes);
  if (swdi != NULL)
    memcpy(swdi, sequence_input, bytes);
}

static dap_process_result_t process(dap_data_t *dap,
                                    const uint8_t *request,
                                    uint32_t request_len,
                                    uint8_t *response,
                                    uint32_t capacity) {
  memset(response, 0xA5, capacity);
  return dap_process_command(dap, request, request_len, response, capacity);
}

static uint32_t load_le32(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void test_info_and_serial(void) {
  dap_data_t dap;
  uint8_t response[64];
  const uint8_t info_version[] = {DAP_CMD_INFO, DAP_INFO_CMSIS_DAP_VER};
  const uint8_t info_serial[] = {DAP_CMD_INFO, DAP_INFO_SER_NUM};
  const uint8_t unsupported[] = {DAP_CMD_JTAG_CONFIGURE};
  dap_process_result_t result;

  reset_mocks();
  dap_init(&dap);
  dap_set_serial("0123456789ABCDEFEXTRA");

  result = process(&dap, info_version, sizeof(info_version),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(response[0] == DAP_CMD_INFO);
  CHECK(strcmp((const char *)&response[2], "2.1.2") == 0);

  result = process(&dap, info_serial, sizeof(info_serial),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(response[1] == 17U);
  CHECK(strcmp((const char *)&response[2], "0123456789ABCDEF") == 0);

  result = process(&dap, unsupported, sizeof(unsupported),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(result.response_len == 1U);
  CHECK(response[0] == DAP_ERROR);
}

static void test_connect_and_configuration(void) {
  dap_data_t dap;
  uint8_t response[64];
  const uint8_t connect[] = {DAP_CMD_CONNECT, DAP_PORT_SWD};
  const uint8_t disconnect[] = {DAP_CMD_DISCONNECT};
  const uint8_t clock[] = {DAP_CMD_SWJ_CLOCK, 0x40, 0x42, 0x0F, 0x00};
  const uint8_t nondivisible_clock[] = {
    DAP_CMD_SWJ_CLOCK, 0xC0U, 0xC6U, 0x2DU, 0x00U
  };
  const uint8_t slow_clock[] = {DAP_CMD_SWJ_CLOCK, 1U, 0U, 0U, 0U};
  const uint8_t fast_clock[] = {
    DAP_CMD_SWJ_CLOCK, 0x00U, 0xE1U, 0xF5U, 0x05U
  };
  const uint8_t bad_clock[] = {DAP_CMD_SWJ_CLOCK, 0, 0, 0, 0};
  dap_process_result_t result;

  reset_mocks();
  dap_init(&dap);

  swd_init_ok = false;
  result = process(&dap, connect, sizeof(connect), response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(response[1] == 0U);
  CHECK(dap.debug_port == 0U);

  swd_init_ok = true;
  result = process(&dap, connect, sizeof(connect), response, sizeof(response));
  CHECK(response[1] == DAP_PORT_SWD);
  CHECK(dap.debug_port == DAP_PORT_SWD);
  CHECK(swd_init_div != 0U);

  result = process(&dap, clock, sizeof(clock), response, sizeof(response));
  CHECK(response[1] == DAP_OK);
  CHECK(swd_set_div ==
        (uint32_t)((uint64_t)RP_CLK_SYS_FREQ * 64U / 1000000U));

  result = process(&dap, nondivisible_clock, sizeof(nondivisible_clock),
                   response, sizeof(response));
  CHECK(response[1] == DAP_OK);
  CHECK(swd_set_div ==
        (uint32_t)(((uint64_t)RP_CLK_SYS_FREQ * 64U + 3000000U - 1U) /
                   3000000U));

  result = process(&dap, slow_clock, sizeof(slow_clock),
                   response, sizeof(response));
  CHECK(response[1] == DAP_OK);
  CHECK(swd_set_div == 0x1000000U);

  result = process(&dap, fast_clock, sizeof(fast_clock),
                   response, sizeof(response));
  CHECK(response[1] == DAP_OK);
  CHECK(swd_set_div == 0x100U);

  result = process(&dap, bad_clock, sizeof(bad_clock),
                   response, sizeof(response));
  CHECK(response[1] == DAP_ERROR);

  result = process(&dap, disconnect, sizeof(disconnect),
                   response, sizeof(response));
  CHECK(response[1] == DAP_OK);
  CHECK(swd_off_count == 1U);
}

static void test_transfer_retry_and_dp_read(void) {
  dap_data_t dap;
  uint8_t response[64];
  const uint8_t configure[] = {
    DAP_CMD_TRANSFER_CONFIGURE, 2U, 2U, 0U, 0U, 0U
  };
  const uint8_t read_dp[] = {
    DAP_CMD_TRANSFER, 0U, 1U, DAP_TRANSFER_RnW
  };
  dap_process_result_t result;

  reset_mocks();
  dap_init(&dap);
  dap.debug_port = DAP_PORT_SWD;
  process(&dap, configure, sizeof(configure), response, sizeof(response));
  script_transfer(SWD_ACK_WAIT, 0U);
  script_transfer(SWD_ACK_OK, 0x12345678U);

  result = process(&dap, read_dp, sizeof(read_dp),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(result.response_len == 7U);
  CHECK(response[1] == 1U);
  CHECK(response[2] == SWD_ACK_OK);
  CHECK(load_le32(&response[3]) == 0x12345678U);
  CHECK(transfer_count == 2U);
}

static void test_posted_ap_reads(void) {
  dap_data_t dap;
  uint8_t response[64];
  const uint8_t request[] = {
    DAP_CMD_TRANSFER, 0U, 2U,
    DAP_TRANSFER_APnDP | DAP_TRANSFER_RnW,
    DAP_TRANSFER_APnDP | DAP_TRANSFER_RnW
  };
  dap_process_result_t result;

  reset_mocks();
  dap_init(&dap);
  dap.debug_port = DAP_PORT_SWD;
  script_transfer(SWD_ACK_OK, 0U);
  script_transfer(SWD_ACK_OK, 0x11111111U);
  script_transfer(SWD_ACK_OK, 0x22222222U);

  result = process(&dap, request, sizeof(request),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(result.response_len == 11U);
  CHECK(response[1] == 2U);
  CHECK(load_le32(&response[3]) == 0x11111111U);
  CHECK(load_le32(&response[7]) == 0x22222222U);
  CHECK(transfer_count == 3U);
}

static void test_match_and_block_transfers(void) {
  dap_data_t dap;
  uint8_t response[64];
  const uint8_t configure[] = {
    DAP_CMD_TRANSFER_CONFIGURE, 0U, 0U, 0U, 1U, 0U
  };
  const uint8_t match[] = {
    DAP_CMD_TRANSFER, 0U, 2U,
    DAP_TRANSFER_MATCH_MASK, 0xFFU, 0xFFU, 0U, 0U,
    DAP_TRANSFER_RnW | DAP_TRANSFER_MATCH_VALUE,
    0x34U, 0x12U, 0U, 0U
  };
  const uint8_t block_read[] = {
    DAP_CMD_TRANSFER_BLOCK, 0U, 2U, 0U, DAP_TRANSFER_RnW
  };
  const uint8_t block_write[] = {
    DAP_CMD_TRANSFER_BLOCK, 0U, 2U, 0U, 0U,
    0x44U, 0x33U, 0x22U, 0x11U,
    0x88U, 0x77U, 0x66U, 0x55U
  };
  dap_process_result_t result;

  reset_mocks();
  dap_init(&dap);
  dap.debug_port = DAP_PORT_SWD;
  process(&dap, configure, sizeof(configure), response, sizeof(response));
  script_transfer(SWD_ACK_OK, 0x00000000U);
  script_transfer(SWD_ACK_OK, 0x00001234U);
  result = process(&dap, match, sizeof(match), response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(response[1] == 2U && response[2] == SWD_ACK_OK);
  CHECK(transfer_count == 2U);

  reset_mocks();
  dap_init(&dap);
  dap.debug_port = DAP_PORT_SWD;
  script_transfer(SWD_ACK_OK, 0x01020304U);
  script_transfer(SWD_ACK_OK, 0xA0B0C0D0U);
  result = process(&dap, block_read, sizeof(block_read),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(result.response_len == 12U);
  CHECK(response[1] == 2U && response[2] == 0U);
  CHECK(response[3] == SWD_ACK_OK);
  CHECK(load_le32(&response[4]) == 0x01020304U);
  CHECK(load_le32(&response[8]) == 0xA0B0C0D0U);

  reset_mocks();
  dap_init(&dap);
  dap.debug_port = DAP_PORT_SWD;
  script_transfer(SWD_ACK_OK, 0U);
  script_transfer(SWD_ACK_OK, 0U);
  script_transfer(SWD_ACK_FAULT, 0U);
  result = process(&dap, block_write, sizeof(block_write),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(result.response_len == 4U);
  CHECK(response[1] == 2U && response[2] == 0U);
  CHECK(response[3] == SWD_ACK_FAULT);
  CHECK(transfers[0].data == 0x11223344U);
  CHECK(transfers[1].data == 0x55667788U);
}

static void test_disconnected_transfer_is_inert(void) {
  dap_data_t dap;
  uint8_t response[64];
  const uint8_t read_dp[] = {
    DAP_CMD_TRANSFER, 0U, 1U, DAP_TRANSFER_RnW
  };
  const uint8_t write_abort[] = {
    DAP_CMD_WRITE_ABORT, 0U, 1U, 0U, 0U, 0U
  };
  const uint8_t block_read[] = {
    DAP_CMD_TRANSFER_BLOCK, 0U, 1U, 0U, DAP_TRANSFER_RnW
  };

  reset_mocks();
  dap_init(&dap);
  CHECK(process(&dap, read_dp, sizeof(read_dp), response,
                sizeof(response)).status == DAP_PROCESS_RESPONSE);
  CHECK(response[1] == 0U && response[2] == 0U);
  CHECK(transfer_count == 0U);

  CHECK(process(&dap, write_abort, sizeof(write_abort), response,
                sizeof(response)).status == DAP_PROCESS_RESPONSE);
  CHECK(response[1] == DAP_ERROR);
  CHECK(transfer_count == 0U);

  CHECK(process(&dap, block_read, sizeof(block_read), response,
                sizeof(response)).status == DAP_PROCESS_RESPONSE);
  CHECK(response[1] == 0U && response[2] == 0U);
  CHECK(response[3] == 0U);
  CHECK(transfer_count == 0U);
}

static void test_sequences(void) {
  dap_data_t dap;
  uint8_t response[64];
  const uint8_t swj[] = {DAP_CMD_SWJ_SEQUENCE, 9U, 0xA5U, 0x01U};
  const uint8_t swd_out[] = {
    DAP_CMD_SWD_SEQUENCE, 1U, 9U, 0x5AU, 0x01U
  };
  const uint8_t swd_in[] = {
    DAP_CMD_SWD_SEQUENCE, 1U, (uint8_t)(0x80U | 9U)
  };

  reset_mocks();
  dap_init(&dap);

  CHECK(process(&dap, swj, sizeof(swj), response,
                sizeof(response)).status == DAP_PROCESS_RESPONSE);
  CHECK(swj_count == 9U);
  CHECK(swj_data[0] == 0xA5U && swj_data[1] == 0x01U);

  CHECK(process(&dap, swd_out, sizeof(swd_out), response,
                sizeof(response)).status == DAP_PROCESS_RESPONSE);
  CHECK(sequence_info == 9U);
  CHECK(sequence_output[0] == 0x5AU && sequence_output[1] == 0x01U);

  sequence_input[0] = 0x3CU;
  sequence_input[1] = 0x01U;
  CHECK(process(&dap, swd_in, sizeof(swd_in), response,
                sizeof(response)).status == DAP_PROCESS_RESPONSE);
  CHECK(response[2] == 0x3CU && response[3] == 0x01U);
}

static void test_execute_and_bounds(void) {
  dap_data_t dap;
  uint8_t response[64];
  const uint8_t execute[] = {
    DAP_CMD_EXECUTE_COMMANDS, 2U,
    DAP_CMD_INFO, DAP_INFO_PACKET_SIZE,
    DAP_CMD_SWJ_CLOCK, 0x40U, 0x42U, 0x0FU, 0x00U
  };
  const uint8_t nested[] = {
    DAP_CMD_EXECUTE_COMMANDS, 1U,
    DAP_CMD_EXECUTE_COMMANDS, 0U
  };
  const uint8_t oversized_block[] = {
    DAP_CMD_TRANSFER_BLOCK, 0U, 16U, 0U, DAP_TRANSFER_RnW
  };
  dap_process_result_t result;

  reset_mocks();
  dap_init(&dap);

  result = process(&dap, execute, sizeof(execute),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(response[0] == DAP_CMD_EXECUTE_COMMANDS);
  CHECK(response[1] == 2U);
  CHECK(response[2] == DAP_CMD_INFO);
  CHECK(response[6] == DAP_CMD_SWJ_CLOCK);

  result = process(&dap, nested, sizeof(nested),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_MALFORMED);
  CHECK(result.response_len == 1U && response[0] == DAP_ERROR);

  result = process(&dap, oversized_block, sizeof(oversized_block),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_MALFORMED);
  CHECK(transfer_count == 0U);
}

static void test_transfer_shape_matches_dispatch(void) {
  dap_data_t dap;
  uint8_t response[64];
  const uint8_t read_with_write_only_flag[] = {
    DAP_CMD_TRANSFER, 0U, 1U,
    DAP_TRANSFER_RnW | DAP_TRANSFER_MATCH_MASK
  };
  const uint8_t execute[] = {
    DAP_CMD_EXECUTE_COMMANDS, 2U,
    DAP_CMD_TRANSFER, 0U, 1U,
    DAP_TRANSFER_RnW | DAP_TRANSFER_MATCH_MASK,
    DAP_CMD_INFO, DAP_INFO_PACKET_COUNT
  };
  dap_process_result_t result;

  reset_mocks();
  dap_init(&dap);
  dap.debug_port = DAP_PORT_SWD;
  script_transfer(SWD_ACK_OK, 0x89ABCDEFU);

  result = process(&dap, read_with_write_only_flag,
                   sizeof(read_with_write_only_flag),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(result.response_len == 7U);
  CHECK(response[1] == 1U && response[2] == SWD_ACK_OK);
  CHECK(load_le32(&response[3]) == 0x89ABCDEFU);

  reset_mocks();
  dap_init(&dap);
  dap.debug_port = DAP_PORT_SWD;
  script_transfer(SWD_ACK_OK, 0x01234567U);

  result = process(&dap, execute, sizeof(execute),
                   response, sizeof(response));
  CHECK(result.status == DAP_PROCESS_RESPONSE);
  CHECK(result.response_len == 12U);
  CHECK(response[0] == DAP_CMD_EXECUTE_COMMANDS);
  CHECK(response[1] == 2U);
  CHECK(response[2] == DAP_CMD_TRANSFER);
  CHECK(load_le32(&response[5]) == 0x01234567U);
  CHECK(response[9] == DAP_CMD_INFO);
  CHECK(response[11] == DAP_PACKET_COUNT);
}

static void test_truncation_and_canaries(void) {
  static const uint8_t examples[][8] = {
    {DAP_CMD_INFO, DAP_INFO_VENDOR},
    {DAP_CMD_HOST_STATUS, 0U, 1U},
    {DAP_CMD_SWJ_CLOCK, 1U, 0U, 0U, 0U},
    {DAP_CMD_TRANSFER_CONFIGURE, 0U, 0U, 0U, 0U, 0U},
    {DAP_CMD_SWJ_PINS, 0U, 0U, 0U, 0U, 0U, 0U},
    {DAP_CMD_TRANSFER, 0U, 1U, DAP_TRANSFER_RnW},
    {DAP_CMD_TRANSFER_BLOCK, 0U, 1U, 0U, DAP_TRANSFER_RnW}
  };
  static const uint8_t lengths[] = {2U, 3U, 5U, 6U, 7U, 4U, 5U};
  dap_data_t dap;
  uint8_t guarded[66];
  size_t e;

  reset_mocks();
  for (e = 0U; e < ARRAY_LEN(examples); e++) {
    uint32_t len;

    for (len = 0U; len < lengths[e]; len++) {
      dap_process_result_t result;

      dap_init(&dap);
      memset(guarded, 0xCC, sizeof(guarded));
      result = dap_process_command(&dap, examples[e], len,
                                   &guarded[1], 64U);
      CHECK(result.status == DAP_PROCESS_MALFORMED);
      CHECK(guarded[0] == 0xCCU && guarded[65] == 0xCCU);
    }
  }
}

static void test_deterministic_packet_fuzz(void) {
  dap_data_t dap;
  uint8_t request[64];
  uint8_t guarded[66];
  uint32_t seed = 0xC0DEC0DEU;
  unsigned iteration;

  reset_mocks();
  for (iteration = 0U; iteration < 20000U; iteration++) {
    uint32_t len;
    uint32_t i;

    seed = seed * 1664525U + 1013904223U;
    len = seed % 65U;
    for (i = 0U; i < sizeof(request); i++) {
      seed = seed * 1664525U + 1013904223U;
      request[i] = (uint8_t)(seed >> 24);
    }
    dap_init(&dap);
    memset(guarded, 0x5A, sizeof(guarded));
    (void)dap_process_command(&dap, request, len, &guarded[1], 64U);
    CHECK(guarded[0] == 0x5AU && guarded[65] == 0x5AU);
  }
}

int main(void) {
  test_info_and_serial();
  test_connect_and_configuration();
  test_transfer_retry_and_dp_read();
  test_posted_ap_reads();
  test_match_and_block_transfers();
  test_disconnected_transfer_is_inert();
  test_sequences();
  test_execute_and_bounds();
  test_transfer_shape_matches_dispatch();
  test_truncation_and_canaries();
  test_deterministic_packet_fuzz();
  puts("DAP unit tests passed");
  return 0;
}
