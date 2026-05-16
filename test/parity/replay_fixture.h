/* test/parity/replay_fixture.h
 *
 * Synthetic input stream for the Stage 15 cross-platform parity
 * test.  A `replay_tick_t` is one tick's worth of BSP readings --
 * the temperature the sensor would have produced and the tach RPM
 * the fan would have spun.  The replay loop (replay_run.c) feeds
 * each tick into thermal_core_step().
 *
 * The fixture is a pure deterministic function of the tick index,
 * not a literal table -- so the host build and the ESP32-C3
 * REPLAY firmware compute byte-identical input from the same C.
 *
 * The stream is a temperature ramp 25 -> 95 -> 25 degC that
 * crosses all four trips of esp32-c3-standalone.json
 * (30 / 45 / 60 / 85 degC) on the way up and again on the way
 * down, exercising the IIR filter, zone aggregation, the
 * step-wise governor, trip hysteresis, and the slew limiter.
 */
#ifndef REPLAY_FIXTURE_H
#define REPLAY_FIXTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t  temp_mc;     /* sensor reading in millidegrees C */
    uint32_t tach_rpm;    /* fan RPM */
} replay_tick_t;

/* Total ticks in the fixture: 10 flat + 141 ramp-up + 10 flat +
 * 141 ramp-down + 10 flat. */
#define G_REPLAY_TICK_COUNT 312

/* Synthetic reading for tick `i` (0 .. G_REPLAY_TICK_COUNT-1). */
replay_tick_t replay_fixture_tick(uint16_t i);

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_FIXTURE_H */
