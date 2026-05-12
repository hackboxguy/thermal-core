/* test/replay/stall_replay.c
 *
 * Stage 6 stall-detector module golden driver. Scenario:
 *   ticks 0-4: pwm=0, tach=0      -- pre-spinup
 *   tick 5:    pwm 0->100, tach=0 -- arms spinup grace (10 ticks)
 *   ticks 5-14: in spinup grace, no fault
 *   tick 15:   spinup over, stall persist counting starts
 *   tick 19:   persist=5 -> DEGRADED
 *   tick 30:   tach 0->500 -- condition clears, -> RECOVERING
 *   tick 33:   recovery_count=3 -> NORMAL
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
    cfg.severity = THERMAL_FAULT_SEVERITY_DEGRADED;
    cfg.action = THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED;
    cfg.persist_ticks = 5;
    cfg.recovery_ticks = 3;
    cfg.threshold0 = 200;      /* stall_rpm */
    cfg.threshold1 = 80;       /* stall_pwm_threshold */
    cfg.correlated_context_id = 0xFFFF;

    thermal_fault_stall_state_t s;
    thermal_fault_stall_reset(&s);

    printf("tick,now_ms,pwm,tach,tach_valid,state,persist_count,recovery_count,spinup_remaining\n");
    for (int t = 0; t < N_TICKS; t++) {
        uint8_t pwm = (t < 5) ? 0u : 100u;
        uint16_t tach = (t < 30) ? 0u : 500u;
        uint8_t valid = 1;
        thermal_fault_stall_step(&s, &cfg, pwm, tach, valid,
                                 /*spinup_pwm*/100, /*spinup_ticks*/10,
                                 (uint32_t)(t * TICK_MS));
        printf("%d,%u,%u,%u,%u,%u,%u,%u,%u\n",
               t, (unsigned)(t * TICK_MS),
               (unsigned)pwm, (unsigned)tach, (unsigned)valid,
               (unsigned)s.state,
               (unsigned)s.persist_count, (unsigned)s.recovery_count,
               (unsigned)s.spinup_remaining);
    }
    return 0;
}
