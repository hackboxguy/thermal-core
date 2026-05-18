/* test/replay/fan_health_replay.c
 *
 * Stage 17 replay driver (PRD Appendix C). Runs a synthetic fan that
 * drifts progressively below its PWM-to-RPM baseline through four
 * phases -- healthy, aging, degraded, failing -- and emits the
 * fan-health detector's published outputs as CSV to stdout.
 *
 * Output is diffed against test/replay/golden/fan_health_drift.csv and
 * against test/reference/fan_health.py (which re-implements the same
 * integer math); all three must match byte-for-byte.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "thermal_fan_health.h"

/* Four operating points held per phase; one tach RPM per (phase,point).
 * Baseline RPM is 1300/2600/3900 -- the phase ratios 1.00 / 0.92 /
 * 0.80 / 0.62 yield clean per-point deltas of 0 / -8 / -20 / -38 %. */
static const uint8_t  PWMS[3]      = { 64, 128, 192 };
static const uint16_t RPM[4][3] = {
    { 1300, 2600, 3900 },   /* healthy   -- delta   0 */
    { 1196, 2392, 3588 },   /* aging     -- delta  -8 */
    { 1040, 2080, 3120 },   /* degraded  -- delta -20 */
    {  806, 1612, 2418 },   /* failing   -- delta -38 */
};
#define TICKS_PER_POINT 40

int main(void) {
    thermal_fan_health_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enable                   = 1;
    cfg.baseline_source          = THERMAL_FAN_BASELINE_SRC_FIELD;
    cfg.baseline[0].x = 64;  cfg.baseline[0].value0 = 1300;
    cfg.baseline[1].x = 128; cfg.baseline[1].value0 = 2600;
    cfg.baseline[2].x = 192; cfg.baseline[2].value0 = 3900;
    cfg.baseline_count           = 3;
    cfg.stable_pwm_ticks         = 3;
    cfg.stable_pwm_tolerance     = 2;
    cfg.stable_rpm_ticks         = 2;
    cfg.stable_rpm_tolerance_pct = 5;
    cfg.min_points_observed      = 2;
    cfg.aging_pct                = -5;
    cfg.degraded_pct             = -15;
    cfg.failing_pct              = -30;

    thermal_fan_health_state_t s;
    thermal_fan_health_reset(&s);

    printf("tick,applied_pwm,tach_rpm,health_delta_pct,severity,confidence\n");
    int tick = 0;
    for (int ph = 0; ph < 4; ph++) {
        for (int p = 0; p < 3; p++) {
            for (int t = 0; t < TICKS_PER_POINT; t++) {
                thermal_fan_health_step(&s, &cfg, PWMS[p], RPM[ph][p],
                                        1, 0, 0, 0);
                printf("%d,%u,%u,%d,%u,%u\n",
                       tick, (unsigned)PWMS[p], (unsigned)RPM[ph][p],
                       (int)s.health_delta_pct, (unsigned)s.severity,
                       (unsigned)s.confidence);
                tick++;
            }
        }
    }
    return 0;
}
