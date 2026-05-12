/* test/replay/stuck_sensor_replay.c
 *
 * Stage 6 stuck-sensor detector module golden driver. Scenario:
 *   sensor = 75000 mc constant, load_changing = 1.
 *   window_ticks = 10, persist_ticks = 1, recovery_ticks = 2.
 *   Tick 9: window completes, max-min=0 < delta=100 with load
 *           changing -> condition fires, persist=1 -> DEGRADED.
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
    cfg.correlated_context_id = 7;   /* configured (not advisory) */

    thermal_fault_stuck_sensor_state_t s;
    thermal_fault_stuck_sensor_reset(&s);

    printf("tick,now_ms,sensor,sensor_valid,load_changing,state,persist_count,recovery_count,window_tick_count,window_delta\n");
    for (int t = 0; t < N_TICKS; t++) {
        int32_t sensor = 75000;
        uint8_t valid = 1;
        uint8_t load = 1;
        thermal_fault_stuck_sensor_step(&s, &cfg, sensor, valid, load,
                                        (uint32_t)(t * TICK_MS));
        int32_t window_delta = s.window_value_max - s.window_value_min;
        printf("%d,%u,%d,%u,%u,%u,%u,%u,%u,%d\n",
               t, (unsigned)(t * TICK_MS),
               sensor, (unsigned)valid, (unsigned)load,
               (unsigned)s.state,
               (unsigned)s.persist_count, (unsigned)s.recovery_count,
               (unsigned)s.window_tick_count,
               window_delta);
    }
    return 0;
}
