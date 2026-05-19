/* test/unit/test_fan_health_telemetry.c
 *
 * Stage 17 fan-health telemetry contracts (PRD Appendix C), exercised
 * end-to-end through thermal_core_step() and the telemetry_emit
 * callback -- the path nothing else covers:
 *
 *   1. A disabled fan-health slot reports baseline-source `none`
 *      (THERMAL_FAN_BASELINE_SRC_NONE), never a zeroed `field`.
 *   2. Once a confident result exists, a long skipped/tach-invalid
 *      period leaves the published severity / confidence standing
 *      unchanged (PRD C.3 -- a skipped tick updates no state).
 */
#include <stdint.h>
#include <string.h>
#include "harness.h"
#include "thermal_core.h"
#include "thermal_signals.h"

/* --- Telemetry capture: last value seen per signal of interest. --- */
static int32_t g_sev, g_conf, g_src, g_delta;

static void cap(uint32_t ts, uint16_t sig, int32_t val) {
    (void)ts;
    if      (sig == TSIG_FAN_HEALTH_SEVERITY(0))        g_sev   = val;
    else if (sig == TSIG_FAN_HEALTH_CONFIDENCE(0))      g_conf  = val;
    else if (sig == TSIG_FAN_HEALTH_BASELINE_SOURCE(0)) g_src   = val;
    else if (sig == TSIG_FAN_HEALTH_DELTA(0))           g_delta = val;
}

static void common_config(thermal_config_t *cfg) {
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
    cfg->actuators[0].slew_per_tick = 255;   /* reach target duty at once */
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
    cfg->zones[0].trips[0].cooling_state = 2;   /* -> state_pwm[2] = 160 */
    cfg->telemetry.enable = 1;
    cfg->telemetry.period_ticks = 1;
    cfg->telemetry.enabled_signal_ids[0] = TSIG_FAN_HEALTH_SEVERITY(0);
    cfg->telemetry.enabled_signal_ids[1] = TSIG_FAN_HEALTH_CONFIDENCE(0);
    cfg->telemetry.enabled_signal_ids[2] = TSIG_FAN_HEALTH_BASELINE_SOURCE(0);
    cfg->telemetry.enabled_signal_ids[3] = TSIG_FAN_HEALTH_DELTA(0);
    cfg->telemetry.enabled_signal_count = 4;
}

/* Build the snapshot and step the core once. */
static void step_once(thermal_core_t *ctx, uint32_t now,
                      int32_t temp_mc, uint16_t tach_rpm, uint8_t tach_valid) {
    thermal_sample_t s[2];
    s[0].id = 0; s[0].kind = THERMAL_SAMPLE_TEMP_MC;
    s[0].valid = 1; s[0].quality = 0;
    s[0].value = temp_mc; s[0].sample_ts_ms = now;
    s[1].id = 0; s[1].kind = THERMAL_SAMPLE_TACH_RPM;
    s[1].valid = tach_valid; s[1].quality = 0;
    s[1].value = (int32_t)tach_rpm; s[1].sample_ts_ms = now;
    thermal_input_snapshot_t snap;
    snap.now_ms = now;
    snap.samples = s;
    snap.sample_count = 2;
    thermal_output_frame_t out;
    memset(&out, 0, sizeof(out));
    EXPECT_EQ(thermal_core_step(ctx, &snap, &out), THERMAL_OK);
}

TEST_CASE(fan_health_telemetry) {
    thermal_config_t cfg;
    thermal_core_t ctx;
    thermal_core_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.telemetry_emit = cap;

    /* === 1. Disabled fan-health slot reports source `none`. ===
     * No fan_health block -> the detector is disabled for actuator 0. */
    common_config(&cfg);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);
    EXPECT_EQ(thermal_core_init(&ctx, &cfg, &cb), THERMAL_OK);
    g_sev = g_conf = g_src = g_delta = -1;
    step_once(&ctx, 0, 75000, 0, 0);
    EXPECT_EQ(g_src, THERMAL_FAN_BASELINE_SRC_NONE);   /* not zeroed `field` */
    EXPECT_EQ(g_sev, THERMAL_FAN_HEALTH_HEALTHY);
    EXPECT_EQ(g_conf, 0);

    /* === 2. A confident result survives a long skipped period. ===
     * Enable the detector; min_points_observed = 1 so one steady
     * operating point is enough to escalate. */
    common_config(&cfg);
    cfg.fan_health[0].enable = 1;
    cfg.fan_health[0].baseline_source = THERMAL_FAN_BASELINE_SRC_FIELD;
    cfg.fan_health[0].baseline[0].x = 80;  cfg.fan_health[0].baseline[0].value0 = 1000;
    cfg.fan_health[0].baseline[1].x = 160; cfg.fan_health[0].baseline[1].value0 = 2000;
    cfg.fan_health[0].baseline[2].x = 255; cfg.fan_health[0].baseline[2].value0 = 3000;
    cfg.fan_health[0].baseline_count = 3;
    cfg.fan_health[0].stable_pwm_ticks = 2;
    cfg.fan_health[0].stable_pwm_tolerance = 4;
    cfg.fan_health[0].stable_rpm_ticks = 1;
    cfg.fan_health[0].stable_rpm_tolerance_pct = 10;
    cfg.fan_health[0].min_points_observed = 1;
    cfg.fan_health[0].aging_pct = -5;
    cfg.fan_health[0].degraded_pct = -15;
    cfg.fan_health[0].failing_pct = -30;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);
    EXPECT_EQ(thermal_core_init(&ctx, &cfg, &cb), THERMAL_OK);

    /* Run the fan steady at duty 160 with tach 1200 (baseline expects
     * 2000 -> -40% -> FAILING) until a confident result lands. */
    for (uint32_t t = 0; t < 8; t++) {
        step_once(&ctx, t * 100u, 75000, 1200, 1);
    }
    EXPECT_EQ(g_src, THERMAL_FAN_BASELINE_SRC_FIELD);
    EXPECT_EQ(g_sev, THERMAL_FAN_HEALTH_FAILING);
    EXPECT_EQ(g_conf, 1);
    int32_t sev_before   = g_sev;
    int32_t conf_before  = g_conf;
    int32_t delta_before = g_delta;

    /* A long tach-invalid period: every tick is skipped, so the
     * published values must stand unchanged (PRD C.3). */
    for (uint32_t t = 8; t < 48; t++) {
        step_once(&ctx, t * 100u, 75000, 0, 0);
    }
    EXPECT_EQ(g_sev,   sev_before);     /* still FAILING */
    EXPECT_EQ(g_conf,  conf_before);
    EXPECT_EQ(g_delta, delta_before);
}
