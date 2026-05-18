/* test/unit/test_fan_health_advisory.c
 *
 * Stage 17 load-bearing invariant (PRD Appendix C): the fan-health
 * detector is advisory-only -- it must never appear in an actuator
 * command path. This test runs the same input sequence through
 * thermal_core_step twice, once with the detector enabled and once
 * disabled, and asserts every output frame is byte-identical.
 */
#include <stdint.h>
#include <string.h>
#include "harness.h"
#include "thermal_core.h"

#define N_TICKS 30

static void build_config(thermal_config_t *cfg, uint8_t fan_health_enable) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->config_version = 1;
    cfg->control_period_ms = 100;
    cfg->sensor_count = 1;
    cfg->sensors[0].id = 0;
    cfg->sensors[0].iir_alpha_q16 = Q16_ONE;
    cfg->sensors[0].max_staleness_ms = 500;
    cfg->actuator_count = 1;
    cfg->actuators[0].id = 0;
    cfg->actuators[0].pwm_min = 80;
    cfg->actuators[0].pwm_max = 255;
    cfg->actuators[0].slew_per_tick = 16;
    cfg->actuators[0].state_pwm[0] = 0;
    cfg->actuators[0].state_pwm[1] = 100;
    cfg->actuators[0].state_pwm[2] = 160;
    cfg->actuators[0].state_pwm[3] = 220;
    cfg->actuators[0].state_pwm[4] = 255;
    cfg->zone_count = 1;
    cfg->zones[0].sensor_count = 1;
    cfg->zones[0].sensor_ids[0] = 0;
    cfg->zones[0].aggregation = THERMAL_AGG_MAX;
    cfg->zones[0].governor = THERMAL_GOVERNOR_STEP_WISE;
    cfg->zones[0].actuator_count = 1;
    cfg->zones[0].actuator_ids[0] = 0;
    cfg->zones[0].fallback_temp_mc = 85000;
    cfg->zones[0].trip_count = 1;
    cfg->zones[0].trips[0].temp_mc = 70000;
    cfg->zones[0].trips[0].hyst_mc = 2000;
    cfg->zones[0].trips[0].severity = THERMAL_TRIP_WARN;
    cfg->zones[0].trips[0].cooling_state = 2;
    cfg->fan_health[0].enable = fan_health_enable;
    cfg->fan_health[0].baseline_source = THERMAL_FAN_BASELINE_SRC_FIELD;
    cfg->fan_health[0].baseline[0].x = 80;  cfg->fan_health[0].baseline[0].value0 = 1000;
    cfg->fan_health[0].baseline[1].x = 160; cfg->fan_health[0].baseline[1].value0 = 2000;
    cfg->fan_health[0].baseline[2].x = 255; cfg->fan_health[0].baseline[2].value0 = 3000;
    cfg->fan_health[0].baseline_count = 3;
    cfg->fan_health[0].stable_pwm_ticks = 2;
    cfg->fan_health[0].stable_pwm_tolerance = 4;
    cfg->fan_health[0].stable_rpm_ticks = 1;
    cfg->fan_health[0].stable_rpm_tolerance_pct = 10;
    cfg->fan_health[0].min_points_observed = 1;
    cfg->fan_health[0].aging_pct = -5;
    cfg->fan_health[0].degraded_pct = -15;
    cfg->fan_health[0].failing_pct = -30;
}

/* Run N_TICKS through the core; capture each tick's output frame.
 * A drifting tach (well below baseline) makes the detector escalate
 * when enabled -- which must still not perturb the frames. */
static void run(uint8_t fan_health_enable, thermal_output_frame_t frames[N_TICKS]) {
    thermal_config_t cfg;
    build_config(&cfg, fan_health_enable);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    thermal_core_t ctx;
    thermal_core_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    EXPECT_EQ(thermal_core_init(&ctx, &cfg, &cb), THERMAL_OK);

    for (uint32_t t = 0; t < N_TICKS; t++) {
        uint32_t now = t * 100u;
        thermal_sample_t s[2];
        s[0].id = 0; s[0].kind = THERMAL_SAMPLE_TEMP_MC;
        s[0].valid = 1; s[0].quality = 0;
        s[0].value = 75000;                       /* above WARN -> fan runs */
        s[0].sample_ts_ms = now;
        s[1].id = 0; s[1].kind = THERMAL_SAMPLE_TACH_RPM;
        s[1].valid = 1; s[1].quality = 0;
        s[1].value = 700;                          /* far below baseline */
        s[1].sample_ts_ms = now;
        thermal_input_snapshot_t snap;
        snap.now_ms = now;
        snap.samples = s;
        snap.sample_count = 2;
        memset(&frames[t], 0, sizeof(frames[t]));
        EXPECT_EQ(thermal_core_step(&ctx, &snap, &frames[t]), THERMAL_OK);
    }
}

TEST_CASE(fan_health_advisory) {
    thermal_output_frame_t on[N_TICKS], off[N_TICKS];
    run(1, on);
    run(0, off);

    /* Every output frame must be byte-identical: the advisory detector
     * cannot reach an actuator command. */
    for (int t = 0; t < N_TICKS; t++) {
        if (memcmp(&on[t], &off[t], sizeof(on[t])) != 0) {
            fprintf(stderr,
                    "FAIL: tick %d output frame differs with fan-health "
                    "enabled vs disabled\n", t);
            exit(1);
        }
    }
}
