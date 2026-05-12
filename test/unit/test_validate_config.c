/* test/unit/test_validate_config.c
 *
 * Unit tests for thermal_core_validate_config. Positive baseline plus
 * one negative perturbation per PRD §5.3 rule implemented in Stage 4
 * commit 3a. Each negative starts from make_valid_config() and modifies
 * one field, so failure messages cleanly identify which rule fired.
 */
#include <stdint.h>
#include <string.h>
#include "harness.h"
#include "thermal_core.h"
#include "thermal_config.h"

static void make_valid_config(thermal_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->config_version = 1;
    cfg->control_period_ms = 100;

    /* One sensor */
    cfg->sensor_count = 1;
    cfg->sensors[0].id = 0;
    cfg->sensors[0].iir_alpha_q16 = 16384;
    cfg->sensors[0].max_staleness_ms = 500;

    /* One actuator */
    cfg->actuator_count = 1;
    cfg->actuators[0].id = 0;
    cfg->actuators[0].pwm_min = 80;
    cfg->actuators[0].pwm_max = 255;
    cfg->actuators[0].state_pwm[0] = 0;
    cfg->actuators[0].state_pwm[1] = 100;
    cfg->actuators[0].state_pwm[2] = 160;
    cfg->actuators[0].state_pwm[3] = 220;
    cfg->actuators[0].state_pwm[4] = 255;

    /* One step-wise zone with two trips */
    cfg->zone_count = 1;
    cfg->zones[0].sensor_count = 1;
    cfg->zones[0].sensor_ids[0] = 0;
    cfg->zones[0].aggregation = THERMAL_AGG_MAX;
    cfg->zones[0].governor = THERMAL_GOVERNOR_STEP_WISE;
    cfg->zones[0].actuator_count = 1;
    cfg->zones[0].actuator_ids[0] = 0;
    cfg->zones[0].fallback_temp_mc = 85000;
    cfg->zones[0].trip_count = 2;
    cfg->zones[0].trips[0].temp_mc = 70000;
    cfg->zones[0].trips[0].hyst_mc = 2000;
    cfg->zones[0].trips[0].severity = THERMAL_TRIP_WARN;
    cfg->zones[0].trips[0].cooling_state = 1;
    cfg->zones[0].trips[1].temp_mc = 90000;
    cfg->zones[0].trips[1].hyst_mc = 2000;
    cfg->zones[0].trips[1].severity = THERMAL_TRIP_CRITICAL;
    cfg->zones[0].trips[1].cooling_state = 3;
}

TEST_CASE(validate_config) {
    thermal_config_t cfg;

    /* === Positive baseline === */
    make_valid_config(&cfg);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* === Rule 1: NULL -> INVALID_ARG === */
    EXPECT_EQ(thermal_core_validate_config(NULL), THERMAL_ERR_INVALID_ARG);

    /* === Rule 2: wrong config_version === */
    make_valid_config(&cfg);
    cfg.config_version = 7;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 3: control_period_ms == 0 === */
    make_valid_config(&cfg);
    cfg.control_period_ms = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 4: sensor_count > MAX === */
    make_valid_config(&cfg);
    cfg.sensor_count = THERMAL_MAX_SENSORS + 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_NO_SPACE);

    /* === Rule 5: zone_count > MAX === */
    make_valid_config(&cfg);
    cfg.zone_count = THERMAL_MAX_ZONES + 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_NO_SPACE);

    /* === Rule 6: actuator_count > MAX === */
    make_valid_config(&cfg);
    cfg.actuator_count = THERMAL_MAX_ACTUATORS + 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_NO_SPACE);

    /* === Rule 7: context_count > MAX === */
    make_valid_config(&cfg);
    cfg.context_count = THERMAL_MAX_CONTEXT_SIGNALS + 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_NO_SPACE);

    /* === Rule 8: modifier_count > MAX === */
    make_valid_config(&cfg);
    cfg.modifier_count = THERMAL_MAX_MODIFIERS + 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_NO_SPACE);

    /* === Rule 9: v1 modifier limit (count > 1, but still <= MAX === 2) === */
    make_valid_config(&cfg);
    cfg.modifier_count = 2;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 10: duplicate sensor IDs === */
    make_valid_config(&cfg);
    cfg.sensor_count = 2;
    cfg.sensors[1].id = 0;                  /* duplicate */
    cfg.sensors[1].iir_alpha_q16 = 16384;
    cfg.sensors[1].max_staleness_ms = 500;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 11: max_staleness_ms == 0 === */
    make_valid_config(&cfg);
    cfg.sensors[0].max_staleness_ms = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 12: duplicate actuator IDs === */
    make_valid_config(&cfg);
    cfg.actuator_count = 2;
    cfg.actuators[1] = cfg.actuators[0];    /* same id */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 13: pwm_min > pwm_max === */
    make_valid_config(&cfg);
    cfg.actuators[0].pwm_min = 200;
    cfg.actuators[0].pwm_max = 100;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 14: spinup_pwm out of [pwm_min, pwm_max] (when spinup_ms > 0) === */
    make_valid_config(&cfg);
    cfg.actuators[0].spinup_ms = 500;
    cfg.actuators[0].spinup_pwm = 50;       /* below pwm_min = 80 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 14: spinup_ms == 0 means spinup_pwm ignored === */
    make_valid_config(&cfg);
    cfg.actuators[0].spinup_ms = 0;
    cfg.actuators[0].spinup_pwm = 50;       /* would be invalid if spinup_ms > 0 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* === Rule 15: duplicate context IDs === */
    make_valid_config(&cfg);
    cfg.context_count = 2;
    cfg.contexts[0].id = 7;
    cfg.contexts[0].unit = THERMAL_CONTEXT_UNIT_KMH;
    cfg.contexts[0].iir_alpha_q16 = 2048;
    cfg.contexts[0].timeout_ms = 3000;
    cfg.contexts[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;
    cfg.contexts[1] = cfg.contexts[0];
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 16: zone sensor_count == 0 === */
    make_valid_config(&cfg);
    cfg.zones[0].sensor_count = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 17: zone actuator_count == 0 === */
    make_valid_config(&cfg);
    cfg.zones[0].actuator_count = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 18: zone references unknown sensor_id === */
    make_valid_config(&cfg);
    cfg.zones[0].sensor_ids[0] = 99;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 19: zone references unknown actuator_id === */
    make_valid_config(&cfg);
    cfg.zones[0].actuator_ids[0] = 99;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 20: unknown aggregation === */
    make_valid_config(&cfg);
    cfg.zones[0].aggregation = 99;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 21: unknown governor === */
    make_valid_config(&cfg);
    cfg.zones[0].governor = 99;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 22: WEIGHTED with zero weight sum === */
    make_valid_config(&cfg);
    cfg.zones[0].aggregation = THERMAL_AGG_WEIGHTED;
    cfg.zones[0].sensor_weights_q16[0] = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 22 positive: WEIGHTED with positive sum is OK */
    make_valid_config(&cfg);
    cfg.zones[0].aggregation = THERMAL_AGG_WEIGHTED;
    cfg.zones[0].sensor_weights_q16[0] = Q16_ONE;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* === Rule 23: trip_count > MAX === */
    make_valid_config(&cfg);
    cfg.zones[0].trip_count = THERMAL_MAX_TRIPS_PER_ZONE + 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_NO_SPACE);

    /* === Rule 24: trips out of order (non-strict equal temps) === */
    make_valid_config(&cfg);
    cfg.zones[0].trips[1].temp_mc = cfg.zones[0].trips[0].temp_mc;  /* equal */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 24 isolated: equal temps with hyst_mc = 0 ===
     * With hyst = 0, rule 25 (cold-side non-overlap) reduces to
     * trip[1].temp_mc >= trip[0].temp_mc, which equal temps satisfy.
     * Rule 24 alone catches the case. Distinguishes strict vs non-strict
     * trip ordering. */
    make_valid_config(&cfg);
    cfg.zones[0].trips[0].hyst_mc = 0;
    cfg.zones[0].trips[1].hyst_mc = 0;
    cfg.zones[0].trips[1].temp_mc = cfg.zones[0].trips[0].temp_mc;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 24: trips descending temp === */
    make_valid_config(&cfg);
    cfg.zones[0].trips[1].temp_mc = 50000;  /* below trips[0] */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 25: hysteresis overlap ===
     * trip[0].temp_mc = 70000. Make trip[1].temp_mc = 71000 and
     * trip[1].hyst_mc = 2000 -> cold side = 69000 < trip[0].temp_mc = 70000. */
    make_valid_config(&cfg);
    cfg.zones[0].trips[1].temp_mc = 71000;
    cfg.zones[0].trips[1].hyst_mc = 2000;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 26: cooling_state >= MAX -> BOUNDS === */
    make_valid_config(&cfg);
    cfg.zones[0].trips[0].cooling_state = THERMAL_MAX_COOLING_STATES;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_BOUNDS);

    /* === Rule 27: unknown severity === */
    make_valid_config(&cfg);
    cfg.zones[0].trips[0].severity = 99;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Telemetry signal count > MAX -> NO_SPACE === */
    make_valid_config(&cfg);
    cfg.telemetry.enabled_signal_count = THERMAL_MAX_TELEMETRY_SIGNALS + 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_NO_SPACE);

    /* === Empty config (only version + period) is OK ===
     * Zero zones / sensors / actuators is technically valid for v1 — the
     * Linux daemon would refuse to operate, but the core doesn't impose
     * a "must have something" rule here. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.config_version = 1;
    cfg.control_period_ms = 100;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* ====================================================================
     * PID-zone rules (Stage 5 commit 5a additions, PRD §5.3 lines 796-798)
     * ==================================================================== */

    /* Build a valid PID-zone config: start from make_valid_config, then
     * switch the zone to PID with full PID config and a critical trip. */
    #define MAKE_PID_CONFIG(c) do {                                            \
        make_valid_config(&(c));                                               \
        (c).zones[0].governor = THERMAL_GOVERNOR_PID;                          \
        (c).zones[0].pid.kp_q16 = 4915;                                        \
        (c).zones[0].pid.ki_q16 = 327;                                         \
        (c).zones[0].pid.kd_q16 = 0;                                           \
        (c).zones[0].pid.setpoint_mc = 75000;                                  \
        (c).zones[0].pid.kp_min_q16 = 0;                                       \
        (c).zones[0].pid.kp_max_q16 = 327680;                                  \
        (c).zones[0].pid.ki_min_q16 = 0;                                       \
        (c).zones[0].pid.ki_max_q16 = 65536;                                   \
        (c).zones[0].pid.kd_min_q16 = 0;                                       \
        (c).zones[0].pid.kd_max_q16 = 65536;                                   \
        (c).zones[0].pid.setpoint_min_mc = 50000;                              \
        (c).zones[0].pid.setpoint_max_mc = 95000;                              \
        (c).zones[0].pid.dt_min_ms = 50;                                       \
        (c).zones[0].pid.dt_max_ms = 500;                                      \
        /* Existing trips: [0]=WARN at 70000, [1]=CRITICAL at 90000.           \
         * The CRITICAL trip already satisfies the trip-floor rule. */         \
    } while (0)

    /* === PID baseline: valid PID zone returns OK === */
    MAKE_PID_CONFIG(cfg);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* === Rule 28: kp_q16 below kp_min_q16 === */
    MAKE_PID_CONFIG(cfg);
    cfg.zones[0].pid.kp_q16 = -1;       /* below kp_min_q16 = 0 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 28: ki_q16 above ki_max_q16 === */
    MAKE_PID_CONFIG(cfg);
    cfg.zones[0].pid.ki_q16 = 1000000;  /* above ki_max_q16 = 65536 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 28: kd_q16 below kd_min_q16 === */
    MAKE_PID_CONFIG(cfg);
    cfg.zones[0].pid.kd_q16 = -100;     /* below kd_min_q16 = 0 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 29: setpoint above bounds === */
    MAKE_PID_CONFIG(cfg);
    cfg.zones[0].pid.setpoint_mc = 100000;  /* above setpoint_max_mc = 95000 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 30: dt_min_ms == 0 === */
    MAKE_PID_CONFIG(cfg);
    cfg.zones[0].pid.dt_min_ms = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 30: dt_min_ms > control_period_ms === */
    MAKE_PID_CONFIG(cfg);
    cfg.zones[0].pid.dt_min_ms = 200;     /* control_period = 100 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 30: dt_max_ms < control_period_ms === */
    MAKE_PID_CONFIG(cfg);
    cfg.zones[0].pid.dt_max_ms = 50;      /* control_period = 100 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 31: PID zone with only WARN trips (no critical/shutdown floor) === */
    MAKE_PID_CONFIG(cfg);
    cfg.zones[0].trips[1].severity = THERMAL_TRIP_WARN;  /* downgrade CRITICAL */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === Rule 31: PID zone with SHUTDOWN trip is OK === */
    MAKE_PID_CONFIG(cfg);
    cfg.zones[0].trips[1].severity = THERMAL_TRIP_SHUTDOWN;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    #undef MAKE_PID_CONFIG
}
