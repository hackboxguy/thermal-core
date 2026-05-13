/* platform/esp32_idf/main/replay_fixture.h
 *
 * Stage 13 13c -- fixture stream for REPLAY_STANDALONE.
 *
 * A `replay_tick_t` is one synthetic `thermal_input_snapshot_t`
 * row's worth of BSP readings -- the temperature the sensor
 * would have produced this tick and the tach RPM the fan would
 * have spun.  app_main_replay() walks the array linearly,
 * one entry per `control_period_ms`, and feeds the values
 * into `thermal_core_step()` exactly the way the standalone
 * loop would have.
 *
 * The host scenario this mirrors is `scenarios/idle_steady_state.scn`
 * (PRD section 9.1 row 1): 10 s at 50000 mc ambient with the fan
 * idle.  At control_period_ms = 100, that's 100 ticks.
 *
 * Stored in `.rodata` thanks to `const`.  Dead-stripped under
 * STANDALONE since nothing references the symbols there.
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

extern const replay_tick_t G_REPLAY_TICKS[];
extern const uint16_t      G_REPLAY_TICK_COUNT;

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_FIXTURE_H */
