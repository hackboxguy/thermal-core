/* test/unit/test_apply_command.c
 *
 * Unit tests for thermal_core_apply_command (Stage 8 7a -> 8a).
 *
 * Single TEST_CASE with labelled sub-scenarios across all 5 v1
 * commands (CMD_SET_PID, CMD_SET_SETPOINT, CMD_SET_TRIP,
 * CMD_SET_CURVE_POINT, CMD_CLEAR_FAULT) + error paths + event
 * emission. Mock callbacks record log_event into a fixed-size array
 * for post-hoc assertions.
 *
 * Cfg shape: 2 sensors, 1 actuator (id 10), 2 zones (zone 0 step-wise
 * with WARN/CRITICAL/SHUTDOWN trips, zone 1 PID with full bounds +
 * CRITICAL floor), 1 acoustic_mask modifier, runaway detector enabled
 * (CLEAR_FAULT target).
 */
#include <stdint.h>
#include <string.h>
#include "harness.h"
#include "thermal_core.h"
#include "thermal_commands.h"
#include "thermal_config.h"
#include "thermal_events.h"
#include "thermal_types.h"

/* === Mock callback recorders ================================== */

#define MOCK_EVENT_CAP 64
typedef struct {
    uint32_t ts; uint16_t code;
    uint32_t a1, a2, a3, a4;
} mock_event_t;
static mock_event_t mock_events[MOCK_EVENT_CAP];
static uint16_t mock_event_count;

static void mock_log_event(uint32_t ts, uint16_t code, uint32_t a1,
                           uint32_t a2, uint32_t a3, uint32_t a4) {
    if (mock_event_count < MOCK_EVENT_CAP) {
        mock_events[mock_event_count].ts = ts;
        mock_events[mock_event_count].code = code;
        mock_events[mock_event_count].a1 = a1;
        mock_events[mock_event_count].a2 = a2;
        mock_events[mock_event_count].a3 = a3;
        mock_events[mock_event_count].a4 = a4;
        mock_event_count++;
    }
}
static void mock_reset(void) { mock_event_count = 0; }
static int count_events(uint16_t code) {
    int n = 0;
    for (uint16_t i = 0; i < mock_event_count; i++) {
        if (mock_events[i].code == code) n++;
    }
    return n;
}

static const thermal_core_callbacks_t MOCK_CB = {
    .log_event = mock_log_event,
    .telemetry_emit = NULL,
};

/* === Config builder ============================================ */

static void build_cfg(thermal_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->config_version = 1;
    cfg->control_period_ms = 100;

    /* Sensors */
    cfg->sensor_count = 2;
    cfg->sensors[0].id = 1;
    cfg->sensors[0].iir_alpha_q16 = Q16_ONE;
    cfg->sensors[0].max_staleness_ms = 1000;
    cfg->sensors[1].id = 2;
    cfg->sensors[1].iir_alpha_q16 = Q16_ONE;
    cfg->sensors[1].max_staleness_ms = 1000;

    /* Context (for modifier) */
    cfg->context_count = 1;
    cfg->contexts[0].id = 100;
    cfg->contexts[0].unit = THERMAL_CONTEXT_UNIT_KMH;
    cfg->contexts[0].iir_alpha_q16 = Q16_ONE;
    cfg->contexts[0].timeout_ms = 500;
    cfg->contexts[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;

    /* Actuator */
    cfg->actuator_count = 1;
    cfg->actuators[0].id = 10;
    cfg->actuators[0].pwm_min = 80;
    cfg->actuators[0].pwm_max = 255;
    cfg->actuators[0].slew_per_tick = 8;
    cfg->actuators[0].state_pwm[0] = 0;
    cfg->actuators[0].state_pwm[1] = 100;
    cfg->actuators[0].state_pwm[2] = 160;
    cfg->actuators[0].state_pwm[3] = 220;
    cfg->actuators[0].state_pwm[4] = 255;

    /* Zone 0: step-wise. */
    cfg->zones[0].sensor_count = 1;
    cfg->zones[0].sensor_ids[0] = 1;
    cfg->zones[0].aggregation = THERMAL_AGG_MAX;
    cfg->zones[0].governor = THERMAL_GOVERNOR_STEP_WISE;
    cfg->zones[0].actuator_count = 1;
    cfg->zones[0].actuator_ids[0] = 10;
    cfg->zones[0].fallback_temp_mc = 50000;
    cfg->zones[0].trip_count = 3;
    cfg->zones[0].trips[0].temp_mc = 70000;
    cfg->zones[0].trips[0].hyst_mc = 2000;
    cfg->zones[0].trips[0].severity = THERMAL_TRIP_WARN;
    cfg->zones[0].trips[0].cooling_state = 2;
    cfg->zones[0].trips[1].temp_mc = 85000;
    cfg->zones[0].trips[1].hyst_mc = 2000;
    cfg->zones[0].trips[1].severity = THERMAL_TRIP_CRITICAL;
    cfg->zones[0].trips[1].cooling_state = 4;
    cfg->zones[0].trips[2].temp_mc = 95000;
    cfg->zones[0].trips[2].hyst_mc = 2000;
    cfg->zones[0].trips[2].severity = THERMAL_TRIP_SHUTDOWN;
    cfg->zones[0].trips[2].cooling_state = 4;

    /* Zone 1: PID with full bounds + CRITICAL floor. */
    cfg->zones[1].sensor_count = 1;
    cfg->zones[1].sensor_ids[0] = 2;
    cfg->zones[1].aggregation = THERMAL_AGG_MAX;
    cfg->zones[1].governor = THERMAL_GOVERNOR_PID;
    cfg->zones[1].actuator_count = 1;
    cfg->zones[1].actuator_ids[0] = 10;
    cfg->zones[1].fallback_temp_mc = 50000;
    cfg->zones[1].pid.kp_q16 = 4915;
    cfg->zones[1].pid.ki_q16 = 327;
    cfg->zones[1].pid.kd_q16 = 0;
    cfg->zones[1].pid.setpoint_mc = 75000;
    cfg->zones[1].pid.kp_min_q16 = 0;
    cfg->zones[1].pid.kp_max_q16 = 327680;
    cfg->zones[1].pid.ki_min_q16 = 0;
    cfg->zones[1].pid.ki_max_q16 = 65536;
    cfg->zones[1].pid.kd_min_q16 = 0;
    cfg->zones[1].pid.kd_max_q16 = 65536;
    cfg->zones[1].pid.setpoint_min_mc = 50000;
    cfg->zones[1].pid.setpoint_max_mc = 95000;
    cfg->zones[1].pid.dt_min_ms = 50;
    cfg->zones[1].pid.dt_max_ms = 500;
    cfg->zones[1].trip_count = 2;
    cfg->zones[1].trips[0].temp_mc = 80000;
    cfg->zones[1].trips[0].hyst_mc = 2000;
    cfg->zones[1].trips[0].severity = THERMAL_TRIP_WARN;
    cfg->zones[1].trips[0].cooling_state = 2;
    cfg->zones[1].trips[1].temp_mc = 90000;
    cfg->zones[1].trips[1].hyst_mc = 2000;
    cfg->zones[1].trips[1].severity = THERMAL_TRIP_CRITICAL;  /* floor */
    cfg->zones[1].trips[1].cooling_state = 4;
    cfg->zone_count = 2;

    /* Modifier */
    cfg->modifier_count = 1;
    strncpy(cfg->modifiers[0].name, "acoustic_mask", THERMAL_NAME_MAX - 1);
    cfg->modifiers[0].context_id = 100;
    cfg->modifiers[0].stages =
        THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET |
        THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP;
    cfg->modifiers[0].curve_count = 4;
    cfg->modifiers[0].curve[0].x = 0;
    cfg->modifiers[0].curve[0].value0 = 120;
    cfg->modifiers[0].curve[0].value1 = 0;
    cfg->modifiers[0].curve[1].x = 30;
    cfg->modifiers[0].curve[1].value0 = 180;
    cfg->modifiers[0].curve[1].value1 = 0;
    cfg->modifiers[0].curve[2].x = 80;
    cfg->modifiers[0].curve[2].value0 = 255;
    cfg->modifiers[0].curve[2].value1 = -5000;
    cfg->modifiers[0].curve[3].x = 130;
    cfg->modifiers[0].curve[3].value0 = 255;
    cfg->modifiers[0].curve[3].value1 = -8000;
    cfg->modifiers[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;

    /* Runaway detector enabled with force_pwm_max action (for CLEAR_FAULT). */
    cfg->faults.runaway_defaults.enabled = 1;
    cfg->faults.runaway_defaults.severity = THERMAL_FAULT_SEVERITY_CRITICAL;
    cfg->faults.runaway_defaults.action =
        THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED;
    cfg->faults.runaway_defaults.persist_ticks = 5;
    cfg->faults.runaway_defaults.recovery_ticks = 5;
    cfg->faults.runaway_defaults.threshold0 = 1000;
    cfg->faults.runaway_defaults.threshold1 = 200;
}

/* === Helpers ================================================== */

static thermal_command_t cmd_set_pid(uint16_t z, int32_t kp, int32_t ki, int32_t kd) {
    thermal_command_t c;
    memset(&c, 0, sizeof(c));
    c.command_id = THERMAL_CMD_SET_PID;
    c.u.set_pid.zone_id = z;
    c.u.set_pid.kp_q16 = kp;
    c.u.set_pid.ki_q16 = ki;
    c.u.set_pid.kd_q16 = kd;
    return c;
}
static thermal_command_t cmd_set_setpoint(uint16_t z, int32_t sp) {
    thermal_command_t c;
    memset(&c, 0, sizeof(c));
    c.command_id = THERMAL_CMD_SET_SETPOINT;
    c.u.set_setpoint.zone_id = z;
    c.u.set_setpoint.setpoint_mc = sp;
    return c;
}
static thermal_command_t cmd_set_trip(uint16_t z, uint16_t t, int32_t temp, int32_t hyst) {
    thermal_command_t c;
    memset(&c, 0, sizeof(c));
    c.command_id = THERMAL_CMD_SET_TRIP;
    c.u.set_trip.zone_id = z;
    c.u.set_trip.trip_idx = t;
    c.u.set_trip.temp_mc = temp;
    c.u.set_trip.hyst_mc = hyst;
    return c;
}
static thermal_command_t cmd_set_curve(uint16_t m, uint16_t p,
                                       int32_t x, int32_t v0, int32_t v1) {
    thermal_command_t c;
    memset(&c, 0, sizeof(c));
    c.command_id = THERMAL_CMD_SET_CURVE_POINT;
    c.u.set_curve_point.modifier_id = m;
    c.u.set_curve_point.point_idx = p;
    c.u.set_curve_point.x = x;
    c.u.set_curve_point.value0 = v0;
    c.u.set_curve_point.value1 = v1;
    return c;
}
static thermal_command_t cmd_clear_fault(uint16_t ftype, uint16_t tid) {
    thermal_command_t c;
    memset(&c, 0, sizeof(c));
    c.command_id = THERMAL_CMD_CLEAR_FAULT;
    c.u.clear_fault.fault_type = ftype;
    c.u.clear_fault.target_id = tid;
    return c;
}

static thermal_sample_t SAMPLES[3];
static void step_t1t2(thermal_core_t *ctx, uint32_t now_ms,
                      int32_t temp1, int32_t temp2,
                      thermal_output_frame_t *out) {
    SAMPLES[0].id = 1; SAMPLES[0].kind = THERMAL_SAMPLE_TEMP_MC;
    SAMPLES[0].valid = 1; SAMPLES[0].value = temp1;
    SAMPLES[1].id = 2; SAMPLES[1].kind = THERMAL_SAMPLE_TEMP_MC;
    SAMPLES[1].valid = 1; SAMPLES[1].value = temp2;
    thermal_input_snapshot_t snap;
    snap.now_ms = now_ms;
    snap.samples = SAMPLES;
    snap.sample_count = 2;
    EXPECT_STATUS_OK(thermal_core_step(ctx, &snap, out));
}

TEST_CASE(apply_command_full_surface) {
    thermal_config_t cfg;
    thermal_core_t ctx;
    thermal_command_result_t r;
    thermal_output_frame_t out;
    thermal_state_snapshot_t state;

    /* ============================================================
     * SET_PID
     * ============================================================ */
    {
        build_cfg(&cfg);
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        mock_reset();

        /* S1: positive on PID zone (zone 1). */
        thermal_command_t c = cmd_set_pid(1, 8192, 500, 100);
        EXPECT_STATUS_OK(thermal_core_apply_command(&ctx, 100, &c, &r));
        EXPECT_EQ(r.status, THERMAL_OK);
        EXPECT_EQ(count_events(TEVENT_COMMAND_APPLIED), 1);
        EXPECT_EQ(count_events(TEVENT_COMMAND_REJECTED), 0);
        EXPECT_EQ(mock_events[0].a1, THERMAL_CMD_SET_PID);
        EXPECT_EQ(mock_events[0].a2, 1);  /* zone_id */
        /* Run a step to confirm the new gains are visible. With temp 75000
         * (= setpoint), error == 0, so PID output is from integrator/D only.
         * After the SET_PID reset the integrator is 0; output_pwm = 0
         * (clamped). Zone 0 is below all trips -> zone 0 demand = 0.
         * Arb = max(0, 0) = 0 -> duty stays 0. The relevant assertion
         * here is just that no error occurs. */
        step_t1t2(&ctx, 200, 50000, 75000, &out);
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 0);

        /* S2: zone_id out of range. */
        mock_reset();
        c = cmd_set_pid(7, 8192, 500, 100);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 300, &c, &r), THERMAL_ERR_INVALID_ARG);
        EXPECT_EQ(count_events(TEVENT_COMMAND_REJECTED), 1);

        /* S3: zone 0 is step-wise -> INVALID_ARG. */
        mock_reset();
        c = cmd_set_pid(0, 8192, 500, 100);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 400, &c, &r), THERMAL_ERR_INVALID_ARG);
        EXPECT_EQ(count_events(TEVENT_COMMAND_REJECTED), 1);

        /* S4: kp > kp_max -> BOUNDS. */
        mock_reset();
        c = cmd_set_pid(1, 1000000, 500, 100);  /* > kp_max_q16 = 327680 */
        EXPECT_EQ(thermal_core_apply_command(&ctx, 500, &c, &r), THERMAL_ERR_BOUNDS);
        EXPECT_EQ(count_events(TEVENT_COMMAND_REJECTED), 1);

        /* S5: ki < ki_min -> BOUNDS. */
        mock_reset();
        c = cmd_set_pid(1, 8192, -1, 100);  /* < ki_min_q16 = 0 */
        EXPECT_EQ(thermal_core_apply_command(&ctx, 600, &c, &r), THERMAL_ERR_BOUNDS);
        EXPECT_EQ(count_events(TEVENT_COMMAND_REJECTED), 1);
    }

    /* ============================================================
     * SET_SETPOINT
     * ============================================================ */
    {
        build_cfg(&cfg);
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        mock_reset();

        /* S6: positive — applied setpoint visible via get_state. */
        thermal_command_t c = cmd_set_setpoint(1, 82000);
        EXPECT_STATUS_OK(thermal_core_apply_command(&ctx, 100, &c, &r));
        EXPECT_EQ(r.status, THERMAL_OK);
        step_t1t2(&ctx, 200, 50000, 75000, &out);
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        EXPECT_EQ(state.zones[1].effective_setpoint_mc, 82000);

        /* S7: zone_id out of range. */
        mock_reset();
        c = cmd_set_setpoint(7, 75000);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 300, &c, &r), THERMAL_ERR_INVALID_ARG);

        /* S8: zone 0 is step-wise. */
        mock_reset();
        c = cmd_set_setpoint(0, 75000);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 400, &c, &r), THERMAL_ERR_INVALID_ARG);

        /* S9: setpoint > max. */
        mock_reset();
        c = cmd_set_setpoint(1, 100000);  /* > setpoint_max_mc = 95000 */
        EXPECT_EQ(thermal_core_apply_command(&ctx, 500, &c, &r), THERMAL_ERR_BOUNDS);
    }

    /* ============================================================
     * SET_TRIP
     * ============================================================ */
    {
        build_cfg(&cfg);
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        mock_reset();

        /* S10: positive — small shift, monotonicity preserved.
         * Zone 0 trip[0] WARN at 70000. Shift to 65000 (hyst 2000).
         * Next trip [1] at 85000-2000=83000 cold side, 65000 < 83000 OK. */
        thermal_command_t c = cmd_set_trip(0, 0, 65000, 2000);
        EXPECT_STATUS_OK(thermal_core_apply_command(&ctx, 100, &c, &r));
        EXPECT_EQ(r.status, THERMAL_OK);
        /* Confirm governor uses new threshold: temp 66000 was below
         * original WARN (70000) but is above new WARN (65000). */
        step_t1t2(&ctx, 200, 66000, 50000, &out);
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        EXPECT_EQ(state.zones[0].active_trip_mask, 1);  /* WARN active */
        EXPECT_EQ(state.zones[0].cooling_state, 2);

        /* S11: zone_id out of range. */
        mock_reset();
        c = cmd_set_trip(7, 0, 65000, 2000);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 300, &c, &r), THERMAL_ERR_INVALID_ARG);

        /* S12: trip_idx out of range. */
        mock_reset();
        c = cmd_set_trip(0, 5, 75000, 2000);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 400, &c, &r), THERMAL_ERR_BOUNDS);

        /* S13: temp <= prev.temp -> BOUNDS.
         * Try to set trip[1] (currently 85000) <= shadow trip[0] (now 65000
         * after S10). Setting trip[1] = 65000 violates strict ordering. */
        mock_reset();
        c = cmd_set_trip(0, 1, 65000, 2000);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 500, &c, &r), THERMAL_ERR_BOUNDS);

        /* S14: hyst cold-side overlap. trip[0] sits at 65000.
         * Try to set trip[1] to 70000 with hyst 6000 (cold side 64000 < 65000). */
        mock_reset();
        c = cmd_set_trip(0, 1, 70000, 6000);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 600, &c, &r), THERMAL_ERR_BOUNDS);
    }

    /* ============================================================
     * SET_CURVE_POINT
     * ============================================================ */
    {
        build_cfg(&cfg);
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        mock_reset();

        /* S15: positive — set point 1 x = 35 (between 0 and 80, was 30). */
        thermal_command_t c = cmd_set_curve(0, 1, 35, 200, 0);
        EXPECT_STATUS_OK(thermal_core_apply_command(&ctx, 100, &c, &r));
        EXPECT_EQ(r.status, THERMAL_OK);

        /* S16: modifier_id out of range. */
        mock_reset();
        c = cmd_set_curve(7, 1, 35, 200, 0);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 200, &c, &r), THERMAL_ERR_INVALID_ARG);

        /* S17: point_idx out of range. */
        mock_reset();
        c = cmd_set_curve(0, 9, 35, 200, 0);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 300, &c, &r), THERMAL_ERR_BOUNDS);

        /* S18: x breaks ascending. point 1 has shadow x=35 now (from S15);
         * point 2 has x=80. Try to set point 1 to x=80 (>= next). */
        mock_reset();
        c = cmd_set_curve(0, 1, 80, 200, 0);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 400, &c, &r), THERMAL_ERR_BOUNDS);
    }

    /* ============================================================
     * CLEAR_FAULT
     * ============================================================ */
    {
        build_cfg(&cfg);
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        mock_reset();

        /* S19: unknown fault_type. */
        thermal_command_t c = cmd_clear_fault(99, 0);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 100, &c, &r), THERMAL_ERR_INVALID_ARG);

        /* S20: NORMAL state -> OK (idempotent). Stall on actuator id 10
         * is currently NORMAL (we haven't run any steps that trip it). */
        mock_reset();
        c = cmd_clear_fault(THERMAL_FAULT_TYPE_STALL, 10);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 200, &c, &r), THERMAL_OK);

        /* S21: drive runaway into CRITICAL, then attempt clear immediately
         * (before recovery_ticks elapse) -> REJECTED_SAFETY.
         *
         * To trigger runaway: temp rising fast while commanded_pwm at the
         * pwm threshold (200). The runaway detector cfg has persist_ticks=5,
         * threshold0=1000 (rise mc), threshold1=200 (cooling_pwm threshold).
         *
         * Drive zone 0 above CRITICAL (temp=88000) so cooling_state=4 ->
         * state_pwm[4]=255, safety_override forces pwm_max=255. Temps rise
         * each tick by 2000 mc -> after 5 ticks the runaway detector sees
         * 5*2000 > 1000 mc rise + pwm 255 >= 200 -> transitions out of NORMAL. */
        mock_reset();
        int32_t t = 86000;
        for (int i = 0; i < 6; i++) {
            step_t1t2(&ctx, (uint32_t)(1000 + i * 100), t, 50000, &out);
            t += 2000;
        }
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        /* At this point runaway is in CRITICAL or LATCHED-ish state (the
         * detector instance is non-NORMAL). target_id for runaway = zone INDEX. */
        c = cmd_clear_fault(THERMAL_FAULT_TYPE_RUNAWAY, 0);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 2000, &c, &r),
                  THERMAL_ERR_REJECTED_SAFETY);

        /* S22: drop temp so the runaway condition clears, step enough ticks
         * for recovery_ticks=5 to elapse, then clear -> OK. Temp dropping
         * and PWM falling below the cooling_pwm threshold (PWM tracks down
         * via slew) ends the runaway condition; recovery_count accumulates. */
        for (int i = 0; i < 30; i++) {
            step_t1t2(&ctx, (uint32_t)(3000 + i * 100), 40000, 40000, &out);
        }
        mock_reset();
        c = cmd_clear_fault(THERMAL_FAULT_TYPE_RUNAWAY, 0);
        EXPECT_EQ(thermal_core_apply_command(&ctx, 6000, &c, &r), THERMAL_OK);
        EXPECT_EQ(count_events(TEVENT_COMMAND_APPLIED), 1);
    }

    /* ============================================================
     * General
     * ============================================================ */
    {
        build_cfg(&cfg);
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        mock_reset();

        /* S23: unknown command_id -> INVALID_ARG, target_id=0 in args. */
        thermal_command_t c;
        memset(&c, 0, sizeof(c));
        c.command_id = 0xFFFF;
        EXPECT_EQ(thermal_core_apply_command(&ctx, 100, &c, &r),
                  THERMAL_ERR_INVALID_ARG);
        EXPECT_EQ(count_events(TEVENT_COMMAND_REJECTED), 1);
        EXPECT_EQ(mock_events[0].a1, 0xFFFF);
        EXPECT_EQ(mock_events[0].a2, 0);

        /* S24: NULL ctx -> INVALID_ARG, no event. */
        mock_reset();
        thermal_command_t c2 = cmd_set_pid(1, 8192, 500, 100);
        EXPECT_EQ(thermal_core_apply_command(NULL, 200, &c2, &r),
                  THERMAL_ERR_INVALID_ARG);
        EXPECT_EQ(mock_event_count, 0);
    }
}
