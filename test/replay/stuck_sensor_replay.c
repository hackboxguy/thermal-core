/* test/replay/stuck_sensor_replay.c
 *
 * Stage 6 stuck-sensor detector module golden driver. Scenario:
 *   sensor = 75000 mc constant, correlated context alternates
 *   between 0 and 100 within each 10-tick window.
 *   window_ticks = 10, correlated_delta_threshold = 50,
 *   persist_ticks = 1, recovery_ticks = 2.
 *   Tick 9: window completes, sensor max-min=0 < delta=100 with
 *           context max-min=100 >= 50 -> condition fires,
 *           persist=1 -> DEGRADED.
 *   Tick 10+: window restarts, condition_active = 0 -> RECOVERING.
 *   Tick 12: recovery_count = 2 -> NORMAL.
 *   Cycle repeats: tick 19 fires again, etc.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "thermal_fault.h"
#include "thermal_types.h"
#include "thermal_core.h"

#define N_TICKS    40
#define TICK_MS    100

int main(void) {
    thermal_fault_detector_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    cfg.severity = THERMAL_FAULT_SEVERITY_DEGRADED;
    cfg.action = THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK;
    cfg.persist_ticks = 1;
    cfg.recovery_ticks = 2;
    cfg.threshold0 = 100;     /* delta_mc */
    cfg.threshold1 = 10;      /* window_ticks */
    cfg.threshold2 = 50;      /* correlated_delta_threshold */
    cfg.correlated_context_id = 7;   /* configured (not advisory) */

    thermal_fault_stuck_sensor_state_t s;
    thermal_fault_stuck_sensor_reset(&s);

    printf("tick,now_ms,sensor,sensor_valid,context_valid,context_value,state,persist_count,recovery_count,window_tick_count,window_delta,context_delta\n");
    for (int t = 0; t < N_TICKS; t++) {
        int32_t sensor = 75000;
        uint8_t valid = 1;
        uint8_t context_valid = 1;
        int32_t context_value = (t % 10 < 5) ? 0 : 100;
        thermal_fault_stuck_sensor_step(&s, &cfg, sensor, valid,
                                        context_valid, context_value,
                                        (uint32_t)(t * TICK_MS));
        int32_t window_delta = s.window_value_max - s.window_value_min;
        int32_t context_delta =
            s.window_context_max - s.window_context_min;
        printf("%d,%u,%d,%u,%u,%d,%u,%u,%u,%u,%d,%d\n",
               t, (unsigned)(t * TICK_MS),
               sensor, (unsigned)valid,
               (unsigned)context_valid, context_value,
               (unsigned)s.state,
               (unsigned)s.persist_count, (unsigned)s.recovery_count,
               (unsigned)s.window_tick_count,
               window_delta, context_delta);
    }
    return 0;
}
