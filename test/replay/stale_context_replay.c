/* test/replay/stale_context_replay.c
 *
 * Stage 6 stale-context detector module golden driver. Scenario:
 *   ticks 0-9: ms_since_last_valid = t * 500 (growing). With
 *              timeout = 3000, condition fires from tick 6 onward
 *              (3000 ms). persist_ticks = 2 -> DEGRADED at tick 7.
 *   tick 10:   context refreshes; ms_since_last_valid drops to 100.
 *              Condition inactive -> RECOVERING.
 *   tick 12:   recovery_count = 2 -> NORMAL.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "thermal_fault.h"
#include "thermal_types.h"
#include "thermal_core.h"

#define N_TICKS    30
#define TICK_MS    100
#define TIMEOUT_MS 3000

int main(void) {
    thermal_fault_detector_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    cfg.severity = THERMAL_FAULT_SEVERITY_DEGRADED;
    cfg.action = THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK;
    cfg.persist_ticks = 2;
    cfg.recovery_ticks = 2;
    cfg.correlated_context_id = 0xFFFF;

    thermal_fault_stale_context_state_t s;
    thermal_fault_stale_context_reset(&s);

    printf("tick,now_ms,ms_since_last_valid,state,persist_count,recovery_count\n");
    for (int t = 0; t < N_TICKS; t++) {
        uint32_t ms_since;
        if (t < 10) {
            ms_since = (uint32_t)(t * 500);     /* 0, 500, 1000, ..., 4500 */
        } else {
            ms_since = 100;                     /* context fresh */
        }
        thermal_fault_stale_context_step(&s, &cfg, ms_since, TIMEOUT_MS,
                                         (uint32_t)(t * TICK_MS));
        printf("%d,%u,%u,%u,%u,%u\n",
               t, (unsigned)(t * TICK_MS),
               (unsigned)ms_since,
               (unsigned)s.state,
               (unsigned)s.persist_count, (unsigned)s.recovery_count);
    }
    return 0;
}
