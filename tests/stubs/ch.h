#ifndef TEST_CH_H
#define TEST_CH_H

#include <stdint.h>

typedef struct {
  uint32_t dummy;
} event_source_t;

typedef uint32_t rtcnt_t;

extern uint32_t test_event_flags;
extern uint32_t test_delay_us;
extern rtcnt_t test_realtime_counter;

static inline void chEvtBroadcastFlags(event_source_t *event,
                                       uint32_t flags) {
  (void)event;
  test_event_flags |= flags;
}

static inline void chThdSleepMicroseconds(uint32_t delay) {
  test_delay_us += delay;
}

static inline rtcnt_t chSysGetRealtimeCounterX(void) {
  return test_realtime_counter++;
}

static inline int chSysIsCounterWithinX(rtcnt_t value, rtcnt_t start,
                                        rtcnt_t end) {
  return (uint32_t)(value - start) < (uint32_t)(end - start);
}

#endif /* TEST_CH_H */
