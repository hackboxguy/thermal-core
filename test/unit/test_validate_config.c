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
#include "thermal_signals.h"

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
    cfg->zones[0].fallback_temp_mc = 90000;
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

#if THERMAL_MAX_MODIFIERS >= 2  /* needs modifier_count=2 <= MAX; skipped under a maxima=1 profile */
    /* === Rule 9: v1 modifier limit (count > 1, but still <= MAX === 2) === */
    make_valid_config(&cfg);
    cfg.modifier_count = 2;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);
#endif

#if THERMAL_MAX_SENSORS >= 2  /* needs a 2-sensor config; skipped under a maxima=1 profile */
    /* === Rule 10: duplicate sensor IDs === */
    make_valid_config(&cfg);
    cfg.sensor_count = 2;
    cfg.sensors[1].id = 0;                  /* duplicate */
    cfg.sensors[1].iir_alpha_q16 = 16384;
    cfg.sensors[1].max_staleness_ms = 500;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);
#endif

    /* === Rule 11: max_staleness_ms == 0 === */
    make_valid_config(&cfg);
    cfg.sensors[0].max_staleness_ms = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

#if THERMAL_MAX_ACTUATORS >= 2  /* needs a 2-actuator config; skipped under a maxima=1 profile */
    /* === Rule 12: duplicate actuator IDs === */
    make_valid_config(&cfg);
    cfg.actuator_count = 2;
    cfg.actuators[1] = cfg.actuators[0];    /* same id */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);
#endif

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

    /* === Rule 14: spinup_ms > 0 with spinup_pwm == 0 is invalid === */
    make_valid_config(&cfg);
    cfg.actuators[0].spinup_ms = 500;
    cfg.actuators[0].spinup_pwm = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === M2: optional period-relative guard must match control period. === */
    make_valid_config(&cfg);
    cfg.period_relative_to_ms = 100;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    make_valid_config(&cfg);
    cfg.period_relative_to_ms = 1000;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === L1: referenced state_pwm[] entries must be non-decreasing. === */
    make_valid_config(&cfg);
    cfg.actuators[0].state_pwm[2] = 90;      /* below state_pwm[1] = 100 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === F7: zero-padded short state_pwm tails are not policy entries. === */
    make_valid_config(&cfg);
    cfg.zones[0].trips[0].cooling_state = 1;
    cfg.zones[0].trips[1].cooling_state = 2;
    cfg.actuators[0].state_pwm[0] = 0;
    cfg.actuators[0].state_pwm[1] = 100;
    cfg.actuators[0].state_pwm[2] = 160;
    cfg.actuators[0].state_pwm[3] = 0;       /* loader zero-padded tail */
    cfg.actuators[0].state_pwm[4] = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

#if THERMAL_MAX_CONTEXT_SIGNALS >= 2  /* needs a 2-context config; skipped under a maxima=1 profile */
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
#endif

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

    /* === L1: fallback_temp_mc must reach the highest CRITICAL trip === */
    make_valid_config(&cfg);
    cfg.zones[0].fallback_temp_mc = 89999;   /* critical trip is 90000 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* === F8: if no CRITICAL/SHUTDOWN trip exists, fallback must reach
     * the highest configured trip so sensor loss still requests cooling. */
    make_valid_config(&cfg);
    cfg.zones[0].trips[1].severity = THERMAL_TRIP_WARN;
    cfg.zones[0].fallback_temp_mc = 89999;   /* highest trip is 90000 */
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

#if THERMALCORE_ENABLE_PID
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
#else
    /* PID enum remains stable, but the governor is unavailable when
     * THERMALCORE_ENABLE_PID is compiled out. */
    MAKE_PID_CONFIG(cfg);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);
#endif

    #undef MAKE_PID_CONFIG

    /* === Rule 32 (Stage 6): runaway persist_ticks > window max -> BOUNDS === */
    make_valid_config(&cfg);
    cfg.faults.runaway_defaults.enabled = 1;
    cfg.faults.runaway_defaults.persist_ticks = THERMAL_FAULT_RUNAWAY_WINDOW_MAX + 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_BOUNDS);

    /* Within bounds: OK */
    make_valid_config(&cfg);
    cfg.faults.runaway_defaults.enabled = 1;
    cfg.faults.runaway_defaults.severity = THERMAL_FAULT_SEVERITY_CRITICAL;
    cfg.faults.runaway_defaults.action = THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED;
    cfg.faults.runaway_defaults.persist_ticks = THERMAL_FAULT_RUNAWAY_WINDOW_MAX;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* Disabled detector: persist_ticks above window is ignored. */
    make_valid_config(&cfg);
    cfg.faults.runaway_defaults.enabled = 0;
    cfg.faults.runaway_defaults.persist_ticks = THERMAL_FAULT_RUNAWAY_WINDOW_MAX + 100;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* ====================================================================
     * Modifier rules (Stage 7 commit 7a, rules 33-35)
     * ==================================================================== */

    #define MAKE_MODIFIER_CONFIG(c) do {                                       \
        make_valid_config(&(c));                                               \
        /* Add a context signal so the modifier's context_id resolves. */      \
        (c).context_count = 1;                                                 \
        (c).contexts[0].id = 0;                                                \
        (c).contexts[0].unit = THERMAL_CONTEXT_UNIT_KMH;                       \
        (c).contexts[0].iir_alpha_q16 = 2048;                                  \
        (c).contexts[0].timeout_ms = 3000;                                     \
        (c).contexts[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;        \
        /* One acoustic_mask modifier with the PRD §5.1 curve. */              \
        (c).modifier_count = 1;                                                \
        strncpy((c).modifiers[0].name, "acoustic_mask",                        \
                THERMAL_NAME_MAX - 1);                                         \
        (c).modifiers[0].context_id = 0;                                       \
        (c).modifiers[0].stages =                                              \
            THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET |                       \
            THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP;                           \
        (c).modifiers[0].curve_count = 4;                                      \
        (c).modifiers[0].curve[0].x = 0;                                       \
        (c).modifiers[0].curve[0].value0 = 120;                                \
        (c).modifiers[0].curve[0].value1 = 0;                                  \
        (c).modifiers[0].curve[1].x = 30;                                      \
        (c).modifiers[0].curve[1].value0 = 180;                                \
        (c).modifiers[0].curve[1].value1 = 0;                                  \
        (c).modifiers[0].curve[2].x = 80;                                      \
        (c).modifiers[0].curve[2].value0 = 255;                                \
        (c).modifiers[0].curve[2].value1 = -5000;                              \
        (c).modifiers[0].curve[3].x = 130;                                     \
        (c).modifiers[0].curve[3].value0 = 255;                                \
        (c).modifiers[0].curve[3].value1 = -8000;                              \
        (c).modifiers[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;       \
    } while (0)

    /* Positive baseline: valid acoustic_mask modifier returns OK. */
    MAKE_MODIFIER_CONFIG(cfg);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* Rule 33: wrong modifier name -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    strncpy(cfg.modifiers[0].name, "acoustic_max", THERMAL_NAME_MAX - 1);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 34: unknown context_id -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.modifiers[0].context_id = 99;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 35: curve_count < 2 -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.modifiers[0].curve_count = 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 35: x not strictly ascending (x[1] == x[0]) -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.modifiers[0].curve[1].x = cfg.modifiers[0].curve[0].x;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 35: descending x -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.modifiers[0].curve[2].x = 10;       /* below previous knot (30) */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 36 (Stage 7 7c): modifier.fail_safe == ASSUME_VALUE -> INVALID_CONFIG.
     * v1 context cfg has no fallback value field to "assume." */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.modifiers[0].fail_safe = THERMAL_FAILSAFE_ASSUME_VALUE;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 36: context.fail_safe == ASSUME_VALUE -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.contexts[0].fail_safe = THERMAL_FAILSAFE_ASSUME_VALUE;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* ====================================================================
     * Codex-C rules 37-49: pre-Stage-9 validate_config tightening
     * ==================================================================== */

    /* Rule 37: unknown context unit -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.contexts[0].unit = 99;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 38: context timeout_ms == 0 -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.contexts[0].timeout_ms = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 39: unknown context fail_safe value (not ASSUME_VALUE which is
     * rule 36, and not in known set) -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.contexts[0].fail_safe = 99;
    cfg.modifiers[0].fail_safe = 99;          /* keep rule 42 consistent */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 40: context alpha < 0 -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.contexts[0].iir_alpha_q16 = -1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 40: context alpha > Q16_ONE -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.contexts[0].iir_alpha_q16 = Q16_ONE + 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 41: sensor alpha > Q16_ONE -> INVALID_CONFIG. */
    make_valid_config(&cfg);
    cfg.sensors[0].iir_alpha_q16 = Q16_ONE + 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 42: modifier fail_safe differs from context fail_safe -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.contexts[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;
    cfg.modifiers[0].fail_safe = THERMAL_FAILSAFE_HOLD_LAST;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 43: modifier stages == 0 -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.modifiers[0].stages = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 43: modifier stages with unknown bit -> INVALID_CONFIG. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.modifiers[0].stages = (uint8_t)(
        THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET | 0x80u);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* H3/L1: modifier pwm_cap values must be in range and honor pwm_min. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.modifiers[0].curve[0].value0 = 50;   /* pwm_min is 80 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_BOUNDS);

    /* H3: modifier trip offsets must keep adjusted trips in int32 range. */
    MAKE_MODIFIER_CONFIG(cfg);
    cfg.modifiers[0].curve[3].value1 = INT32_MAX;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_BOUNDS);

    /* Rule 44: enabled fault detector with unknown severity -> INVALID_CONFIG. */
    make_valid_config(&cfg);
    cfg.faults.stall_defaults.enabled = 1;
    cfg.faults.stall_defaults.severity = 99;
    cfg.faults.stall_defaults.action = THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 45: enabled fault detector with unknown action -> INVALID_CONFIG. */
    make_valid_config(&cfg);
    cfg.faults.runaway_defaults.enabled = 1;
    cfg.faults.runaway_defaults.severity = THERMAL_FAULT_SEVERITY_CRITICAL;
    cfg.faults.runaway_defaults.action = 99;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 46: stuck_sensor correlated_context_id refers to a missing context. */
    make_valid_config(&cfg);
    cfg.faults.stuck_sensor_defaults.enabled = 1;
    cfg.faults.stuck_sensor_defaults.severity = THERMAL_FAULT_SEVERITY_DEGRADED;
    cfg.faults.stuck_sensor_defaults.action = THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK;
    cfg.faults.stuck_sensor_defaults.correlated_context_id = 77;  /* not configured */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 46: configured correlated context requires a positive context delta. */
    make_valid_config(&cfg);
    cfg.context_count = 1;
    cfg.contexts[0].id = 7;
    cfg.contexts[0].unit = THERMAL_CONTEXT_UNIT_KMH;
    cfg.contexts[0].iir_alpha_q16 = 2048;
    cfg.contexts[0].timeout_ms = 3000;
    cfg.contexts[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;
    cfg.faults.stuck_sensor_defaults.enabled = 1;
    cfg.faults.stuck_sensor_defaults.severity = THERMAL_FAULT_SEVERITY_DEGRADED;
    cfg.faults.stuck_sensor_defaults.action = THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK;
    cfg.faults.stuck_sensor_defaults.correlated_context_id = 7;
    cfg.faults.stuck_sensor_defaults.threshold2 = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 46: configured correlated context + positive delta -> OK. */
    cfg.faults.stuck_sensor_defaults.threshold2 = 5;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* Rule 46: 0xFFFF advisory -> OK. */
    make_valid_config(&cfg);
    cfg.faults.stuck_sensor_defaults.enabled = 1;
    cfg.faults.stuck_sensor_defaults.severity = THERMAL_FAULT_SEVERITY_DEGRADED;
    cfg.faults.stuck_sensor_defaults.action = THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK;
    cfg.faults.stuck_sensor_defaults.correlated_context_id = 0xFFFF;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* Rule 47: state_pwm[cs] referenced by a trip is non-zero but
     * below pwm_min -> BOUNDS. */
    make_valid_config(&cfg);
    /* trips[0].cooling_state = 1 maps to state_pwm[1] = 100 (>= pwm_min=80
     * -- valid). Mutate state_pwm[1] to 50 (below pwm_min). */
    cfg.actuators[0].state_pwm[1] = 50;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_BOUNDS);

    /* Rule 47: state_pwm[cs] = 0 (off) is always legal even below pwm_min. */
    make_valid_config(&cfg);
    cfg.actuators[0].state_pwm[1] = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* Rule 48: telemetry.enable=1 with period_ticks=0 -> INVALID_CONFIG. */
    make_valid_config(&cfg);
    cfg.telemetry.enable = 1;
    cfg.telemetry.period_ticks = 0;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 49: enabled_signal_ids[i] outside any known range -> INVALID_CONFIG. */
    make_valid_config(&cfg);
    cfg.telemetry.enable = 1;
    cfg.telemetry.period_ticks = 1;
    cfg.telemetry.enabled_signal_count = 1;
    cfg.telemetry.enabled_signal_ids[0] = 0x0a00;   /* no range covers this */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 49: enabled_signal_ids[i] in zone range but slot out of bounds. */
    make_valid_config(&cfg);
    cfg.telemetry.enable = 1;
    cfg.telemetry.period_ticks = 1;
    cfg.telemetry.enabled_signal_count = 1;
    cfg.telemetry.enabled_signal_ids[0] = TSIG_ZONE_TEMP(5);  /* zone_count=1 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Rule 49 positive: valid range/slot OK. */
    make_valid_config(&cfg);
    cfg.telemetry.enable = 1;
    cfg.telemetry.period_ticks = 1;
    cfg.telemetry.enabled_signal_count = 2;
    cfg.telemetry.enabled_signal_ids[0] = TSIG_ZONE_TEMP(0);
    cfg.telemetry.enabled_signal_ids[1] = TSIG_ACTUATOR_DUTY(0);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    #undef MAKE_MODIFIER_CONFIG

#if THERMALCORE_ENABLE_FAN_HEALTH
    /* ====================================================================
     * Fan-health rules (Stage 17, PRD Appendix C) -- validate_fan_health
     * ==================================================================== */

    #define MAKE_FAN_HEALTH_CONFIG(c) do {                                 \
        make_valid_config(&(c));                                           \
        thermal_fan_health_cfg_t *fh = &(c).fan_health[0];                 \
        fh->enable = 1;                                                    \
        fh->baseline_source = THERMAL_FAN_BASELINE_SRC_FIELD;              \
        fh->baseline[0].x = 64;  fh->baseline[0].value0 = 900;             \
        fh->baseline[1].x = 128; fh->baseline[1].value0 = 1850;            \
        fh->baseline[2].x = 255; fh->baseline[2].value0 = 2900;            \
        fh->baseline_count = 3;                                            \
        fh->stable_pwm_ticks = 300;  fh->stable_pwm_tolerance = 2;         \
        fh->stable_rpm_ticks = 50;   fh->stable_rpm_tolerance_pct = 5;     \
        fh->min_points_observed = 2;                                       \
        fh->aging_pct = -5; fh->degraded_pct = -15; fh->failing_pct = -30; \
    } while (0)

    /* Positive baseline: a valid fan_health block returns OK. */
    MAKE_FAN_HEALTH_CONFIG(cfg);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* A one-point baseline is rejected (needs >= 2 points). */
    MAKE_FAN_HEALTH_CONFIG(cfg);
    cfg.fan_health[0].baseline_count = 1;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Baseline PWM not strictly ascending. */
    MAKE_FAN_HEALTH_CONFIG(cfg);
    cfg.fan_health[0].baseline[1].x = 64;        /* equals baseline[0].x */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Baseline RPM not monotonic non-decreasing. */
    MAKE_FAN_HEALTH_CONFIG(cfg);
    cfg.fan_health[0].baseline[1].value0 = 800;  /* below baseline[0].value0 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Severity thresholds violate 0 > aging > degraded > failing. */
    MAKE_FAN_HEALTH_CONFIG(cfg);
    cfg.fan_health[0].aging_pct = -20;           /* not > degraded (-15) */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* min_points_observed past the baseline point count. */
    MAKE_FAN_HEALTH_CONFIG(cfg);
    cfg.fan_health[0].min_points_observed = 4;   /* baseline_count = 3 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* A disabled block carries no baseline and is not validated. */
    MAKE_FAN_HEALTH_CONFIG(cfg);
    cfg.fan_health[0].enable = 0;
    cfg.fan_health[0].baseline_count = 0;        /* would fail if validated */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    /* baseline_source outside FIELD/FACTORY/MODEL. */
    MAKE_FAN_HEALTH_CONFIG(cfg);
    cfg.fan_health[0].baseline_source = 99;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* stable_rpm_tolerance_pct above 100. */
    MAKE_FAN_HEALTH_CONFIG(cfg);
    cfg.fan_health[0].stable_rpm_tolerance_pct = 150;
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    /* Baseline's highest PWM point below the actuator's pwm_max. */
    MAKE_FAN_HEALTH_CONFIG(cfg);
    cfg.fan_health[0].baseline[2].x = 200;       /* pwm_max is 255 */
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_ERR_INVALID_CONFIG);

    #undef MAKE_FAN_HEALTH_CONFIG
#endif /* THERMALCORE_ENABLE_FAN_HEALTH */
}
