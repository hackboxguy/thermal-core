/* test/replay/runaway_replay.c
 *
 * Stage 6 runaway-detector module golden driver. Scenario:
 *   ticks 0-9: temp ramps 80000 -> 80900 (rise=900 >= threshold=500),
 *              pwm=220 (>= cooling_threshold=200). Window persist=10
 *              fills at tick 9; condition fires; action=
 *              FORCE_PWM_MAX_AND_LATCH -> LATCHED.
 *   ticks 10+: temp stays at 80900, pwm drops to 100. Condition
 *              inactive, recovery_count ticks toward recovery_ticks=5.
 *              clear_allowed returns 1 at tick 14.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "thermal_fault.h"
#include "thermal_types.h"
#include "thermal_core.h"

#define N_TICKS    50
#define TICK_MS    100

int main(void) {
    thermal_fault_detector_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    cfg.severity = THERMAL_FAULT_SEVERITY_CRITICAL;
    cfg.action = THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH;
    cfg.persist_ticks = 10;
    cfg.recovery_ticks = 5;
    cfg.threshold0 = 500;      /* rise_mc_threshold */
    cfg.threshold1 = 200;      /* cooling_pwm_threshold */
    cfg.correlated_context_id = 0xFFFF;

    thermal_fault_runaway_state_t s;
    thermal_fault_runaway_reset(&s);

    printf("tick,now_ms,temp,pwm,window_filled,state,persist_count,recovery_count\n");
    for (int t = 0; t < N_TICKS; t++) {
        int32_t temp;
        uint8_t pwm;
        if (t < 10) {
            temp = 80000 + (int32_t)t * 100;
            pwm = 220;
        } else {
            temp = 80900;
            pwm = 100;
        }
        thermal_fault_runaway_step(&s, &cfg, temp, pwm,
                                   (uint32_t)(t * TICK_MS));
        printf("%d,%u,%d,%u,%u,%u,%u,%u\n",
               t, (unsigned)(t * TICK_MS),
               temp, (unsigned)pwm,
               (unsigned)s.window_filled,
               (unsigned)s.state,
               (unsigned)s.persist_count, (unsigned)s.recovery_count);
    }
    return 0;
}
