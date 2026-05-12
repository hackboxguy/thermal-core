/* test/unit/test_core_step.c
 *
 * End-to-end unit tests for thermal_core_step() — Stage 7 commit 7c.
 *
 * One TEST_CASE with labeled sub-scenarios. The scenarios mandated by
 * implementation-plan §5 Stage 7 line 285 are explicitly named so a
 * refactor cannot silently delete them:
 *
 *   S4  CRITICAL trip bypasses acoustic cap
 *   S5  SHUTDOWN trip bypasses cap + emits TEVENT_SHUTDOWN_REQUEST
 *   S6  Slew limit obeyed on downward transition
 *   S7  Slew bypassed on upward safety override
 *
 * Plus complementary coverage:
 *   S1  Below all trips -> duty = 0 (0-stays-0 final clamp)
 *   S2  WARN + acoustic cap reduces request
 *   S3  Two-zone arbitration (max-wins)
 *   S8  Context stale -> fail-safe STATIONARY drives modifier
 *
 * Mock callbacks record log_event + telemetry_emit into fixed-size
 * arrays for post-hoc assertions.
 */
#include <stdint.h>
#include <string.h>
#include "harness.h"
#include "thermal_core.h"
#include "thermal_commands.h"
#include "thermal_config.h"
#include "thermal_events.h"
#include "thermal_signals.h"

/* === Mock callback recorders ================================== */

#define MOCK_EVENT_CAP 64
typedef struct {
    uint32_t ts; uint16_t code;
    uint32_t a1, a2, a3, a4;
} mock_event_t;
static mock_event_t mock_events[MOCK_EVENT_CAP];
static uint16_t mock_event_count;

#define MOCK_TLM_CAP 256
typedef struct {
    uint32_t ts; uint16_t sig; int32_t val;
} mock_tlm_t;
static mock_tlm_t mock_tlms[MOCK_TLM_CAP];
static uint16_t mock_tlm_count;

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
static void mock_telemetry_emit(uint32_t ts, uint16_t sig, int32_t val) {
    if (mock_tlm_count < MOCK_TLM_CAP) {
        mock_tlms[mock_tlm_count].ts = ts;
        mock_tlms[mock_tlm_count].sig = sig;
        mock_tlms[mock_tlm_count].val = val;
        mock_tlm_count++;
    }
}
static void mock_reset(void) {
    mock_event_count = 0;
    mock_tlm_count = 0;
}
static int count_events(uint16_t code) {
    int n = 0;
    for (uint16_t i = 0; i < mock_event_count; i++) {
        if (mock_events[i].code == code) n++;
    }
    return n;
}

static const thermal_core_callbacks_t MOCK_CB = {
    .log_event = mock_log_event,
    .telemetry_emit = mock_telemetry_emit,
};

/* === Config builders ========================================== */

/* Base: 1 sensor, 1 actuator, 1 step-wise zone with WARN @70°C cs=2,
 * CRITICAL @85°C cs=4, SHUTDOWN @95°C cs=4. pwm_max=255, pwm_min=80,
 * slew=8. state_pwm linear. */
static void build_base_cfg(thermal_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->config_version = 1;
    cfg->control_period_ms = 100;

    cfg->sensor_count = 1;
    cfg->sensors[0].id = 1;
    cfg->sensors[0].iir_alpha_q16 = Q16_ONE;   /* pass-through */
    cfg->sensors[0].max_staleness_ms = 1000;

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

    cfg->zone_count = 1;
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
}

/* Append the PRD §5.1 acoustic_mask modifier + speed context. */
static void add_acoustic_mask(thermal_config_t *cfg) {
    cfg->context_count = 1;
    cfg->contexts[0].id = 100;
    cfg->contexts[0].unit = THERMAL_CONTEXT_UNIT_KMH;
    cfg->contexts[0].iir_alpha_q16 = Q16_ONE;
    cfg->contexts[0].timeout_ms = 500;
    cfg->contexts[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;

    cfg->modifier_count = 1;
    strncpy(cfg->modifiers[0].name, "acoustic_mask", THERMAL_NAME_MAX - 1);
    cfg->modifiers[0].context_id = 100;
    cfg->modifiers[0].stages =
        THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET |
        THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP;
    cfg->modifiers[0].curve_count = 4;
    cfg->modifiers[0].curve[0].x = 0;   cfg->modifiers[0].curve[0].value0 = 120;
    cfg->modifiers[0].curve[0].value1 = 0;
    cfg->modifiers[0].curve[1].x = 30;  cfg->modifiers[0].curve[1].value0 = 180;
    cfg->modifiers[0].curve[1].value1 = 0;
    cfg->modifiers[0].curve[2].x = 80;  cfg->modifiers[0].curve[2].value0 = 255;
    cfg->modifiers[0].curve[2].value1 = -5000;
    cfg->modifiers[0].curve[3].x = 130; cfg->modifiers[0].curve[3].value0 = 255;
    cfg->modifiers[0].curve[3].value1 = -8000;
    cfg->modifiers[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;
}

/* === Step driver helpers ====================================== */

static thermal_sample_t SAMPLES[4];

static void run_step(thermal_core_t *ctx, uint32_t now_ms,
                     int32_t temp1_mc, uint8_t temp1_valid,
                     int32_t temp2_mc, uint8_t temp2_valid,
                     int32_t speed_kmh, uint8_t speed_valid,
                     uint8_t sample_count,
                     thermal_output_frame_t *out) {
    SAMPLES[0].id = 1;
    SAMPLES[0].kind = THERMAL_SAMPLE_TEMP_MC;
    SAMPLES[0].valid = temp1_valid;
    SAMPLES[0].value = temp1_mc;
    SAMPLES[1].id = 2;
    SAMPLES[1].kind = THERMAL_SAMPLE_TEMP_MC;
    SAMPLES[1].valid = temp2_valid;
    SAMPLES[1].value = temp2_mc;
    SAMPLES[2].id = 100;
    SAMPLES[2].kind = THERMAL_SAMPLE_CONTEXT_I32;
    SAMPLES[2].valid = speed_valid;
    SAMPLES[2].value = speed_kmh;
    thermal_input_snapshot_t snap;
    snap.now_ms = now_ms;
    snap.samples = SAMPLES;
    snap.sample_count = sample_count;
    EXPECT_STATUS_OK(thermal_core_step(ctx, &snap, out));
}

/* Convenience: 1 temp sample, 1 context sample. */
static void step_t1c(thermal_core_t *ctx, uint32_t now_ms,
                     int32_t temp_mc, int32_t speed_kmh,
                     thermal_output_frame_t *out) {
    SAMPLES[0].id = 1;
    SAMPLES[0].kind = THERMAL_SAMPLE_TEMP_MC;
    SAMPLES[0].valid = 1;
    SAMPLES[0].value = temp_mc;
    SAMPLES[1].id = 100;
    SAMPLES[1].kind = THERMAL_SAMPLE_CONTEXT_I32;
    SAMPLES[1].valid = 1;
    SAMPLES[1].value = speed_kmh;
    thermal_input_snapshot_t snap;
    snap.now_ms = now_ms;
    snap.samples = SAMPLES;
    snap.sample_count = 2;
    EXPECT_STATUS_OK(thermal_core_step(ctx, &snap, out));
}

/* Step with only 1 temp sample (no context). */
static void step_t1(thermal_core_t *ctx, uint32_t now_ms,
                    int32_t temp_mc, thermal_output_frame_t *out) {
    SAMPLES[0].id = 1;
    SAMPLES[0].kind = THERMAL_SAMPLE_TEMP_MC;
    SAMPLES[0].valid = 1;
    SAMPLES[0].value = temp_mc;
    thermal_input_snapshot_t snap;
    snap.now_ms = now_ms;
    snap.samples = SAMPLES;
    snap.sample_count = 1;
    EXPECT_STATUS_OK(thermal_core_step(ctx, &snap, out));
}

TEST_CASE(core_step_full_loop) {
    thermal_config_t cfg;
    thermal_core_t ctx;
    thermal_output_frame_t out;
    thermal_state_snapshot_t state;

    /* ============================================================
     * S1 -- Below all trips: cooling_state=0 -> duty=0 (0-stays-0).
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        step_t1(&ctx, 100, 50000, &out);
        EXPECT_EQ(out.actuator_cmd_count, 1);
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 0);
        EXPECT_EQ(out.actuator_cmds[0].reason, THERMAL_ACT_REASON_NONE);
    }

    /* ============================================================
     * S2 -- WARN trip + acoustic cap reduces request.
     * Temp 72°C -> WARN active, cs=2, state_pwm[2]=160. Speed=0 ->
     * modifier curve.value0 at x=0 is 120 -> cap to 120. With slew=8
     * from prev=0, first tick clamps up to 8 (since requested 120 is
     * above prev+slew). Walk many ticks until steady state at 120.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        for (uint32_t i = 0; i < 30; i++) {
            step_t1c(&ctx, 100 + i * 100, 72000, 0, &out);
        }
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 120);
        EXPECT_EQ(out.actuator_cmds[0].reason,
                  THERMAL_ACT_REASON_MODIFIER_ACOUSTIC_CAP);
    }

    /* ============================================================
     * S3 -- Two-zone max-wins arbitration. Two zones reference the
     * same actuator. Zone A at 72°C -> cs=2 -> 160; Zone B at 86°C
     * -> cs=4 -> 255. Arbitrated request before cap/slew is 255.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        /* Add a second sensor + zone driving the same actuator (id=10). */
        cfg.sensor_count = 2;
        cfg.sensors[1].id = 2;
        cfg.sensors[1].iir_alpha_q16 = Q16_ONE;
        cfg.sensors[1].max_staleness_ms = 1000;
        cfg.zone_count = 2;
        cfg.zones[1] = cfg.zones[0];           /* clone zone 0 structurally */
        cfg.zones[1].sensor_ids[0] = 2;
        /* Both zones drive actuator 10. */

        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        /* Many ticks so slew + clamp reach steady-state. zone B at 86°C
         * makes it CRITICAL (state_pwm[4]=255). Final duty pinned at
         * pwm_max=255 because CRITICAL severity sets safety_override_up
         * which bypasses slew upward. */
        for (uint32_t i = 0; i < 40; i++) {
            run_step(&ctx, 100 + i * 100,
                     72000, 1,    /* sensor 1 */
                     86000, 1,    /* sensor 2 */
                     0, 0,        /* no context */
                     2,           /* only 2 samples */
                     &out);
        }
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
    }

    /* ============================================================
     * S4 -- CRITICAL trip bypasses acoustic cap.  (impl-plan §5 named)
     * Temp 86°C -> CRITICAL active. cs=4, state_pwm[4]=255 -> demand
     * 255. Modifier configured for cap=120 (speed=0) but PRD §4.7
     * skips cap on CRITICAL zone. slew_override_up bypasses slew up,
     * so duty reaches 255 in one tick. CRITICAL alone does NOT force
     * pwm_max -- the demand is already 255 in this cfg so the visible
     * duty matches, but reason is GOVERNOR_STEP not FAULT_RUNAWAY.
     * Codex review #4 fix.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        step_t1c(&ctx, 100, 86000, 0, &out);
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
        EXPECT_EQ(out.actuator_cmds[0].reason, THERMAL_ACT_REASON_GOVERNOR_STEP);
        /* TEVENT_SAFETY_OVERRIDE emitted on rising edge of slew_override_up. */
        EXPECT_EQ(count_events(TEVENT_SAFETY_OVERRIDE), 1);
    }

    /* ============================================================
     * S5 -- SHUTDOWN trip bypasses cap, requests max, emits event
     * exactly once.  (impl-plan §5 named)
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        /* Two ticks at SHUTDOWN: TEVENT_SHUTDOWN_REQUEST must fire once
         * (rising edge), not twice. */
        step_t1c(&ctx, 100, 96000, 0, &out);
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
        EXPECT_EQ(out.actuator_cmds[0].reason, THERMAL_ACT_REASON_SAFETY_SHUTDOWN);
        EXPECT_EQ(count_events(TEVENT_SHUTDOWN_REQUEST), 1);
        step_t1c(&ctx, 200, 96000, 0, &out);
        EXPECT_EQ(count_events(TEVENT_SHUTDOWN_REQUEST), 1); /* still 1 */
    }

    /* ============================================================
     * S6 -- Slew limit obeyed on downward transition. (impl-plan §5)
     * Walk temp up to CRITICAL (override bypasses slew up, immediate
     * pwm_max=255). Then drop temp far below all trips: cooling demand
     * is 0 but slew limits downward step to 255-8 = 247, then 239,
     * etc. PRD §4.6 line 598: "recovery and downward transitions still
     * obey slew limits."
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        step_t1(&ctx, 100, 96000, &out);     /* SHUTDOWN -> duty=255 */
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
        step_t1(&ctx, 200, 30000, &out);     /* drop to cool */
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 247);
        step_t1(&ctx, 300, 30000, &out);
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 239);
        step_t1(&ctx, 400, 30000, &out);
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 231);
    }

    /* ============================================================
     * S7 -- Slew bypassed on upward safety override. (impl-plan §5)
     * Walk up to WARN (cs=2, state_pwm[2]=160). After many ticks at
     * 72°C, duty is ramped to a low value -- but to keep this test
     * predictable, we step once from a known prev. Then jump to
     * SHUTDOWN temp: safety_override_up should bypass slew and the
     * duty jumps to pwm_max in one tick.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        /* First tick: zero cooling, duty=0. */
        step_t1(&ctx, 100, 50000, &out);
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 0);
        /* Single tick into SHUTDOWN. slew=8 normally would limit
         * prev=0 -> 8 only. Override bypasses -> 255. */
        step_t1(&ctx, 200, 96000, &out);
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
    }

    /* ============================================================
     * S8 -- Context stale -> fail-safe STATIONARY drives modifier.
     * Send a valid speed sample once (speed=120), then stop sending it.
     * After context.timeout_ms (500 ms) elapses, modifier consumes
     * value 0 (STATIONARY) instead of the held 120.
     *
     * At speed=120 the modifier curve.value0 ~= 255 (relaxed cap), and
     * value1 (trip_offset) ~= -7400 -> WARN trip effectively at 62.6°C.
     * At speed=0 the cap is 120 and trip_offset is 0 -> WARN at 70°C.
     *
     * We test: at temp 65°C with stale speed, modifier uses speed=0 so
     * WARN is NOT triggered (since trip effective at 70°C). At fresh
     * speed=120, WARN-equivalent shift makes 65°C above the offset
     * trip -> cs >= 2 and cap is relaxed.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        /* Tick 1: feed speed=120 valid. Temp 65°C, with offset -7400
         * effective WARN trip = 62.6°C -> WARN active, cs=2.
         * Speed=120 -> pwm_cap ~= 255 (relaxed) -> no effective cap. */
        step_t1c(&ctx, 100, 65000, 120, &out);
        /* Tick 2: still fresh; same. Many ticks to settle slew. */
        for (uint32_t i = 0; i < 30; i++) {
            step_t1c(&ctx, 200 + i * 100, 65000, 120, &out);
        }
        uint8_t duty_fresh = out.actuator_cmds[0].duty_0_255;
        /* duty_fresh should be 160 (state_pwm[2]) and reason
         * GOVERNOR_STEP (no cap kicked in). */
        EXPECT_EQ(duty_fresh, 160);

        /* Now drop context for 10 ticks (timeout=500ms; 10*100=1000ms).
         * Per STATIONARY fail-safe, modifier value -> 0, trip_offset
         * -> 0, cap -> 120. At temp 65°C with no offset, WARN trip
         * (at 70°C) is NOT active. cooling_state = 0. duty wants 0.
         * Slew limits it down 160 -> 152 -> 144 -> ... eventually 0. */
        for (uint32_t i = 0; i < 40; i++) {
            /* 1 temp sample only, no context. */
            run_step(&ctx, 3200 + i * 100,
                     65000, 1,
                     0, 0,
                     0, 0,
                     1, &out);
        }
        /* After plenty of slew steps, duty must be 0 (passed through
         * pwm_min via 0-stays-0 special case). */
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 0);
    }

    /* ============================================================
     * Telemetry cadence: emit once per tick when period_ticks=1.
     * Configure 2 enabled signals (zone temp + actuator pwm) and
     * verify both fire on each of 3 ticks.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        cfg.telemetry.enable = 1;
        cfg.telemetry.period_ticks = 1;
        cfg.telemetry.enabled_signal_count = 2;
        cfg.telemetry.enabled_signal_ids[0] = TSIG_ZONE_TEMP_SOC;
        cfg.telemetry.enabled_signal_ids[1] = TSIG_ACTUATOR_PWM_MAIN_FAN;
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        for (uint32_t i = 0; i < 3; i++) {
            step_t1(&ctx, 100 + i * 100, 50000, &out);
        }
        EXPECT_EQ(mock_tlm_count, 6); /* 2 signals × 3 ticks */
    }

    /* ============================================================
     * S9 -- CRITICAL trip with state_pwm[cs] < pwm_max.
     * Verifies CRITICAL alone does NOT force pwm_max (codex #4).
     * Build a cfg whose CRITICAL trip maps to cs=3 -> state_pwm[3]=220.
     * Temp triggers CRITICAL -> demand=220, NOT 255.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        cfg.zones[0].trips[1].cooling_state = 3;  /* was 4; state_pwm[3]=220 */
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        step_t1(&ctx, 100, 86000, &out);
        /* CRITICAL active, cs=3 -> state_pwm[3]=220. slew_override_up=1
         * bypasses slew up, so duty reaches 220 in one tick. */
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 220);
        EXPECT_EQ(out.actuator_cmds[0].reason, THERMAL_ACT_REASON_GOVERNOR_STEP);
    }

    /* ============================================================
     * S10 -- PID safety floor applied in arbitration.
     * PID zone with low PID output but active CRITICAL trip.
     * Demand must be max(pid_pwm, state_pwm[critical_floor_cs]) >=
     * floor, not the raw PID value. Codex #5.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        /* Swap zone 0 to PID, keep the same trips (which already have a
         * CRITICAL @ 85°C, cs=4 -> state_pwm[4]=255). */
        cfg.zones[0].governor = THERMAL_GOVERNOR_PID;
        cfg.zones[0].pid.kp_q16 = 1;          /* near-zero -> tiny PID demand */
        cfg.zones[0].pid.ki_q16 = 0;
        cfg.zones[0].pid.kd_q16 = 0;
        cfg.zones[0].pid.setpoint_mc = 88000;  /* setpoint above critical */
        cfg.zones[0].pid.kp_min_q16 = 0;
        cfg.zones[0].pid.kp_max_q16 = 65536;
        cfg.zones[0].pid.ki_min_q16 = 0;
        cfg.zones[0].pid.ki_max_q16 = 65536;
        cfg.zones[0].pid.kd_min_q16 = 0;
        cfg.zones[0].pid.kd_max_q16 = 65536;
        cfg.zones[0].pid.setpoint_min_mc = 50000;
        cfg.zones[0].pid.setpoint_max_mc = 95000;
        cfg.zones[0].pid.dt_min_ms = 50;
        cfg.zones[0].pid.dt_max_ms = 500;
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        /* Temp 86000 < setpoint 88000 -> PID error negative -> PID
         * output saturates low (~0). But CRITICAL trip is active
         * (>=85000), cs=4 -> floor state_pwm[4]=255. Demand=255. */
        step_t1(&ctx, 100, 86000, &out);
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
    }

    /* ============================================================
     * S11 -- REQUEST_SHUTDOWN fault action emits shutdown event and
     * forces pwm_max. Configure runaway with action=request_shutdown
     * and drive zone into a runaway condition. Codex #6.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        cfg.faults.runaway_defaults.enabled = 1;
        cfg.faults.runaway_defaults.severity = THERMAL_FAULT_SEVERITY_CRITICAL;
        cfg.faults.runaway_defaults.action = THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN;
        cfg.faults.runaway_defaults.persist_ticks = 3;
        cfg.faults.runaway_defaults.recovery_ticks = 5;
        cfg.faults.runaway_defaults.threshold0 = 500;   /* rise_mc */
        cfg.faults.runaway_defaults.threshold1 = 200;   /* cooling_pwm */
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        /* Push temp upward fast at high PWM. SHUTDOWN trip at 95000
         * also active -- avoid that by capping temp below 95000 so
         * the shutdown_action path is the one we're testing. */
        int32_t t = 86000;
        for (int i = 0; i < 8; i++) {
            step_t1(&ctx, (uint32_t)(1000 + i * 100), t, &out);
            if (t < 93000) t += 600;
        }
        /* Runaway has latched into a non-NORMAL state with action
         * REQUEST_SHUTDOWN -> shutdown_action triggers force_pwm_max
         * + TEVENT_SHUTDOWN_REQUEST + reason SAFETY_SHUTDOWN. */
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
        EXPECT_EQ(out.actuator_cmds[0].reason, THERMAL_ACT_REASON_SAFETY_SHUTDOWN);
        /* At least one shutdown event from the runaway request_shutdown
         * action (zone-level SHUTDOWN trip not reached in this run). */
        if (count_events(TEVENT_SHUTDOWN_REQUEST) < 1) {
            EXPECT_EQ(count_events(TEVENT_SHUTDOWN_REQUEST), 1);
        }
    }

    /* ============================================================
     * S12 (tightened, Codex v2-#2): USE_ZONE_FALLBACK action on
     * stuck_sensor with a correlated context configured. Sensor flat
     * for window+persist ticks -> detector latches -> zone temp swaps
     * to fallback_temp_mc -> governor SEES the fallback (this was the
     * bug from Commit A: zr->temp_mc had the fallback but governor
     * still received agg.temp_mc).
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        cfg.zones[0].fallback_temp_mc = 87000;  /* >=CRITICAL trip(85000) */
        cfg.faults.stuck_sensor_defaults.enabled = 1;
        cfg.faults.stuck_sensor_defaults.severity =
            THERMAL_FAULT_SEVERITY_DEGRADED;
        cfg.faults.stuck_sensor_defaults.action =
            THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK;
        cfg.faults.stuck_sensor_defaults.persist_ticks = 1;
        cfg.faults.stuck_sensor_defaults.recovery_ticks = 3;
        cfg.faults.stuck_sensor_defaults.threshold0 = 5;    /* delta_mc */
        cfg.faults.stuck_sensor_defaults.threshold1 = 5;    /* window_ticks */
        cfg.faults.stuck_sensor_defaults.correlated_context_id = 100;
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        /* Send valid speed context + flat sensor for many ticks. */
        for (int i = 0; i < 20; i++) {
            step_t1c(&ctx, (uint32_t)(100 + i * 100), 50000, 60, &out);
        }
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        /* Detector latched -> fallback applied. Zone temp = 87000;
         * cooling_state reflects fallback (CRITICAL trip at 85000 is
         * active, cs=4). */
        EXPECT_EQ(state.zones[0].temp_mc, 87000);
        EXPECT_EQ(state.zones[0].cooling_state, 4);
        /* Fault count includes the stuck_sensor entry. */
        int stuck_active = 0;
        for (uint8_t i = 0; i < state.fault_count; i++) {
            if (state.faults[i].fault_type == THERMAL_FAULT_TYPE_STUCK_SENSOR) {
                stuck_active = 1;
                break;
            }
        }
        EXPECT_EQ(stuck_active, 1);
    }

    /* ============================================================
     * S13 -- Absent sensor invalidation. Codex #1.
     * Tick 1: send valid sample -> filter valid.
     * Tick 2: send no samples -> filter should mark valid=0 and zone
     * aggregation uses fallback_temp_mc.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        cfg.zones[0].fallback_temp_mc = 88000;
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        step_t1(&ctx, 100, 60000, &out);
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        EXPECT_EQ(state.zones[0].temp_mc, 60000);
        /* Tick 2 with no samples (NULL + count=0). */
        thermal_input_snapshot_t empty;
        empty.now_ms = 200;
        empty.samples = NULL;
        empty.sample_count = 0;
        EXPECT_STATUS_OK(thermal_core_step(&ctx, &empty, &out));
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        /* Filter valid=0 -> aggregation falls back to fallback_temp_mc. */
        EXPECT_EQ(state.zones[0].temp_mc, 88000);
    }

    /* ============================================================
     * S14 -- Snapshot preflight: NULL samples + sample_count>0 ->
     * INVALID_ARG, state unchanged. Codex #2.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        step_t1(&ctx, 100, 60000, &out);
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        int32_t baseline_temp = state.zones[0].temp_mc;
        uint32_t baseline_now = state.now_ms;
        thermal_input_snapshot_t bad;
        bad.now_ms = 200;
        bad.samples = NULL;
        bad.sample_count = 1;
        EXPECT_EQ(thermal_core_step(&ctx, &bad, &out),
                  THERMAL_ERR_INVALID_ARG);
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        EXPECT_EQ(state.zones[0].temp_mc, baseline_temp);
        EXPECT_EQ(state.now_ms, baseline_now);
    }

    /* ============================================================
     * S15 -- Snapshot preflight: duplicate (kind, id) -> INVALID_ARG,
     * state unchanged. Codex #2.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        step_t1(&ctx, 100, 60000, &out);
        thermal_sample_t dup_samples[2];
        dup_samples[0].id = 1;
        dup_samples[0].kind = THERMAL_SAMPLE_TEMP_MC;
        dup_samples[0].valid = 1;
        dup_samples[0].value = 75000;
        dup_samples[1] = dup_samples[0];           /* duplicate (kind, id) */
        thermal_input_snapshot_t snap;
        snap.now_ms = 200;
        snap.samples = dup_samples;
        snap.sample_count = 2;
        EXPECT_EQ(thermal_core_step(&ctx, &snap, &out),
                  THERMAL_ERR_INVALID_ARG);
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        /* Temperature unchanged from baseline (60000 from previous step,
         * not the 75000 the duplicate tried to set). */
        EXPECT_EQ(state.zones[0].temp_mc, 60000);
    }

    /* ============================================================
     * S16 -- Snapshot preflight: unknown sensor id -> INVALID_ARG.
     * Codex #2.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        thermal_sample_t bad_id;
        bad_id.id = 99;        /* unknown sensor */
        bad_id.kind = THERMAL_SAMPLE_TEMP_MC;
        bad_id.valid = 1;
        bad_id.value = 60000;
        thermal_input_snapshot_t snap;
        snap.now_ms = 100;
        snap.samples = &bad_id;
        snap.sample_count = 1;
        EXPECT_EQ(thermal_core_step(&ctx, &snap, &out),
                  THERMAL_ERR_INVALID_ARG);
    }

    /* ============================================================
     * S17 -- Uninitialized context returns THERMAL_ERR_STATE on all
     * public APIs that depend on cfg. Codex #7.
     * ============================================================ */
    {
        thermal_core_t fresh;
        memset(&fresh, 0, sizeof(fresh));   /* zero-init, never thermal_core_init */
        thermal_sample_t s0;
        s0.id = 1; s0.kind = THERMAL_SAMPLE_TEMP_MC;
        s0.valid = 1; s0.value = 50000;
        thermal_input_snapshot_t snap;
        snap.now_ms = 100;
        snap.samples = &s0;
        snap.sample_count = 1;
        EXPECT_EQ(thermal_core_step(&fresh, &snap, &out), THERMAL_ERR_STATE);
        thermal_command_t c;
        memset(&c, 0, sizeof(c));
        c.command_id = THERMAL_CMD_SET_PID;
        thermal_command_result_t r;
        EXPECT_EQ(thermal_core_apply_command(&fresh, 100, &c, &r),
                  THERMAL_ERR_STATE);
        EXPECT_EQ(thermal_core_get_state(&fresh, &state), THERMAL_ERR_STATE);
    }

    /* ============================================================
     * S18 (codex v2-#1) -- tach plumbing + full-loop stall.
     * Feed valid tach=0 with arb_pwm at pwm_max after spin-up grace;
     * stall detector should latch + force pwm_max + reason FAULT_STALL.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        cfg.actuators[0].spinup_ms = 0;  /* no spinup grace */
        cfg.faults.stall_defaults.enabled = 1;
        cfg.faults.stall_defaults.severity = THERMAL_FAULT_SEVERITY_CRITICAL;
        cfg.faults.stall_defaults.action =
            THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED;
        cfg.faults.stall_defaults.persist_ticks = 2;
        cfg.faults.stall_defaults.recovery_ticks = 2;
        cfg.faults.stall_defaults.threshold0 = 100;  /* stall_rpm */
        cfg.faults.stall_defaults.threshold1 = 50;   /* stall_pwm_threshold */
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        /* Drive temp into CRITICAL so PWM ramps to 255; provide valid
         * tach=0 (stalled) each tick. */
        thermal_sample_t s[2];
        thermal_input_snapshot_t snap;
        for (int i = 0; i < 10; i++) {
            s[0].id = 1; s[0].kind = THERMAL_SAMPLE_TEMP_MC;
            s[0].valid = 1; s[0].value = 90000;
            s[1].id = 10; s[1].kind = THERMAL_SAMPLE_TACH_RPM;
            s[1].valid = 1; s[1].value = 0;
            snap.now_ms = (uint32_t)(100 + i * 100);
            snap.samples = s;
            snap.sample_count = 2;
            EXPECT_STATUS_OK(thermal_core_step(&ctx, &snap, &out));
        }
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
        EXPECT_EQ(out.actuator_cmds[0].reason, THERMAL_ACT_REASON_FAULT_STALL);
        /* RPM reported in state snapshot. */
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        EXPECT_EQ(state.actuators[0].rpm, 0);
        EXPECT_EQ(state.actuators[0].tach_valid, 1);
    }

    /* ============================================================
     * S19 (codex v2-#3) -- stuck-sensor with FORCE_PWM_MAX action
     * pushes affected actuators to pwm_max.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        cfg.faults.stuck_sensor_defaults.enabled = 1;
        cfg.faults.stuck_sensor_defaults.severity =
            THERMAL_FAULT_SEVERITY_CRITICAL;
        cfg.faults.stuck_sensor_defaults.action =
            THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED;
        cfg.faults.stuck_sensor_defaults.persist_ticks = 1;
        cfg.faults.stuck_sensor_defaults.recovery_ticks = 3;
        cfg.faults.stuck_sensor_defaults.threshold0 = 5;
        cfg.faults.stuck_sensor_defaults.threshold1 = 5;
        cfg.faults.stuck_sensor_defaults.correlated_context_id = 100;
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        for (int i = 0; i < 20; i++) {
            step_t1c(&ctx, (uint32_t)(100 + i * 100), 50000, 60, &out);
        }
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
        /* Codex-C v3-#7: distinct reason code for stuck-sensor (was
         * borrowing FAULT_RUNAWAY before this commit). */
        EXPECT_EQ(out.actuator_cmds[0].reason,
                  THERMAL_ACT_REASON_FAULT_STUCK_SENSOR);
    }

    /* ============================================================
     * S20 (codex v2-#3) -- stale-context with FORCE_PWM_MAX action.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        cfg.faults.stale_context_defaults.enabled = 1;
        cfg.faults.stale_context_defaults.severity =
            THERMAL_FAULT_SEVERITY_CRITICAL;
        cfg.faults.stale_context_defaults.action =
            THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED;
        cfg.faults.stale_context_defaults.persist_ticks = 2;
        cfg.faults.stale_context_defaults.recovery_ticks = 2;
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        /* First valid context to mark ever_valid, then omit context for
         * > timeout_ms to fire stale detection. */
        step_t1c(&ctx, 100, 50000, 60, &out);
        for (int i = 0; i < 15; i++) {
            step_t1(&ctx, (uint32_t)(200 + i * 100), 50000, &out);
        }
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
        /* Codex-C v3-#7: distinct reason for stale-context. */
        EXPECT_EQ(out.actuator_cmds[0].reason,
                  THERMAL_ACT_REASON_FAULT_STALE_CONTEXT);
    }

    /* ============================================================
     * S21 (codex v2-#4) -- THERMAL_STATE_SHUTDOWN_REQUESTED latches
     * persistently across ticks after the triggering runaway has
     * already cleared.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        cfg.faults.runaway_defaults.enabled = 1;
        cfg.faults.runaway_defaults.severity = THERMAL_FAULT_SEVERITY_CRITICAL;
        cfg.faults.runaway_defaults.action =
            THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN;
        cfg.faults.runaway_defaults.persist_ticks = 3;
        cfg.faults.runaway_defaults.recovery_ticks = 5;
        cfg.faults.runaway_defaults.threshold0 = 500;
        cfg.faults.runaway_defaults.threshold1 = 200;
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        int32_t t = 86000;
        for (int i = 0; i < 8; i++) {
            step_t1(&ctx, (uint32_t)(100 + i * 100), t, &out);
            if (t < 93000) t += 600;
        }
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        EXPECT_EQ((state.flags & THERMAL_STATE_SHUTDOWN_REQUESTED) != 0, 1);
        /* Now drop temp far below all trips. Even after runaway clears
         * (and zone severity drops below SHUTDOWN), the latch stays. */
        for (int i = 0; i < 50; i++) {
            step_t1(&ctx, (uint32_t)(2000 + i * 100), 40000, &out);
        }
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        EXPECT_EQ((state.flags & THERMAL_STATE_SHUTDOWN_REQUESTED) != 0, 1);
    }

    /* ============================================================
     * S22 (codex v1-#11 / v2-#5) -- stale-context fault sets both
     * ANY_CONTEXT_STALE AND ANY_FAULT_ACTIVE.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        cfg.faults.stale_context_defaults.enabled = 1;
        cfg.faults.stale_context_defaults.severity =
            THERMAL_FAULT_SEVERITY_DEGRADED;
        cfg.faults.stale_context_defaults.action =
            THERMAL_FAULT_ACTION_MARK_DEGRADED;
        cfg.faults.stale_context_defaults.persist_ticks = 2;
        cfg.faults.stale_context_defaults.recovery_ticks = 2;
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        step_t1c(&ctx, 100, 50000, 60, &out);
        for (int i = 0; i < 15; i++) {
            step_t1(&ctx, (uint32_t)(200 + i * 100), 50000, &out);
        }
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        EXPECT_EQ((state.flags & THERMAL_STATE_ANY_CONTEXT_STALE) != 0, 1);
        EXPECT_EQ((state.flags & THERMAL_STATE_ANY_FAULT_ACTIVE) != 0, 1);
    }

    /* ============================================================
     * S23 (codex v3-#6) -- stuck-sensor REQUEST_SHUTDOWN action emits
     * TEVENT_SHUTDOWN_REQUEST on the detector's rising edge, mirroring
     * stall/runaway/stale-context behavior.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        cfg.faults.stuck_sensor_defaults.enabled = 1;
        cfg.faults.stuck_sensor_defaults.severity =
            THERMAL_FAULT_SEVERITY_CRITICAL;
        cfg.faults.stuck_sensor_defaults.action =
            THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN;
        cfg.faults.stuck_sensor_defaults.persist_ticks = 1;
        cfg.faults.stuck_sensor_defaults.recovery_ticks = 3;
        cfg.faults.stuck_sensor_defaults.threshold0 = 5;
        cfg.faults.stuck_sensor_defaults.threshold1 = 5;
        cfg.faults.stuck_sensor_defaults.correlated_context_id = 100;
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        for (int i = 0; i < 20; i++) {
            step_t1c(&ctx, (uint32_t)(100 + i * 100), 50000, 60, &out);
        }
        /* Exactly one TEVENT_SHUTDOWN_REQUEST emitted across the run
         * (rising edge once; latch keeps state but does not re-emit). */
        EXPECT_EQ(count_events(TEVENT_SHUTDOWN_REQUEST), 1);
        /* Reason is SAFETY_SHUTDOWN (shutdown_action path overrides). */
        EXPECT_EQ(out.actuator_cmds[0].reason, THERMAL_ACT_REASON_SAFETY_SHUTDOWN);
        /* State flag latched. */
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        EXPECT_EQ((state.flags & THERMAL_STATE_SHUTDOWN_REQUESTED) != 0, 1);
    }

    /* ============================================================
     * S24 (codex v3-#4) -- multi-sensor stuck-sensor "max of remaining
     * valid sensors". Two-sensor zone: one sensor stuck flat (50000),
     * other sensor warm (78000 → above WARN trip). Zone temp should
     * track the warm sensor, NOT the fallback. cooling_state = 2
     * (WARN active via 78000).
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        /* Two sensors, both feeding zone 0. */
        cfg.sensor_count = 2;
        cfg.sensors[1].id = 2;
        cfg.sensors[1].iir_alpha_q16 = Q16_ONE;
        cfg.sensors[1].max_staleness_ms = 1000;
        cfg.zones[0].sensor_count = 2;
        cfg.zones[0].sensor_ids[0] = 1;
        cfg.zones[0].sensor_ids[1] = 2;
        cfg.zones[0].fallback_temp_mc = 95000;  /* would force SHUTDOWN if used */
        /* state_pwm[0] must remain valid for new rule 47. */
        cfg.actuators[0].state_pwm[0] = 0;
        cfg.faults.stuck_sensor_defaults.enabled = 1;
        cfg.faults.stuck_sensor_defaults.severity =
            THERMAL_FAULT_SEVERITY_DEGRADED;
        cfg.faults.stuck_sensor_defaults.action =
            THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK;
        cfg.faults.stuck_sensor_defaults.persist_ticks = 1;
        cfg.faults.stuck_sensor_defaults.recovery_ticks = 3;
        cfg.faults.stuck_sensor_defaults.threshold0 = 5;
        cfg.faults.stuck_sensor_defaults.threshold1 = 5;
        cfg.faults.stuck_sensor_defaults.correlated_context_id = 100;
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        /* sensor 1 stuck at 50000 (flat -> detector flags as stuck);
         * sensor 2 warm-and-varying around 78000 so its window delta
         * exceeds threshold and the detector keeps it NORMAL. */
        for (int i = 0; i < 25; i++) {
            int32_t s2 = 78000 + ((i % 3) * 100);  /* 78000, 78100, 78200 */
            run_step(&ctx, (uint32_t)(100 + i * 100),
                     50000, 1,    /* sensor 1: flat */
                     s2, 1,       /* sensor 2: varying */
                     60, 1,
                     3, &out);
        }
        EXPECT_STATUS_OK(thermal_core_get_state(&ctx, &state));
        /* Zone temp tracks the warm sensor (varying around 78000), not
         * the fallback (95000). Detector latched sensor 1 only. */
        EXPECT_EQ(state.zones[0].temp_mc >= 78000 &&
                  state.zones[0].temp_mc < 79000, 1);
        EXPECT_EQ(state.zones[0].cooling_state, 2);  /* WARN active */
        /* Sensor 1 is latched as stuck. */
        int stuck_count = 0;
        for (uint8_t i = 0; i < state.fault_count; i++) {
            if (state.faults[i].fault_type == THERMAL_FAULT_TYPE_STUCK_SENSOR) {
                stuck_count++;
            }
        }
        EXPECT_EQ(stuck_count, 1);
    }

    /* ============================================================
     * S25 (codex v3-#1) -- range-based telemetry dispatch across two
     * zones and two actuators. Verifies the new TSIG_*(slot) macros
     * decode to the right state values; not just slot-0 named aliases.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        /* Add a second zone + sensor + actuator to exercise slot 1. */
        cfg.sensor_count = 2;
        cfg.sensors[1].id = 2;
        cfg.sensors[1].iir_alpha_q16 = Q16_ONE;
        cfg.sensors[1].max_staleness_ms = 1000;
        cfg.actuator_count = 2;
        cfg.actuators[1] = cfg.actuators[0];
        cfg.actuators[1].id = 11;
        cfg.zone_count = 2;
        cfg.zones[1] = cfg.zones[0];
        cfg.zones[1].sensor_ids[0] = 2;
        cfg.zones[1].actuator_ids[0] = 11;
        cfg.telemetry.enable = 1;
        cfg.telemetry.period_ticks = 1;
        cfg.telemetry.enabled_signal_count = 6;
        cfg.telemetry.enabled_signal_ids[0] = TSIG_ZONE_TEMP(0);
        cfg.telemetry.enabled_signal_ids[1] = TSIG_ZONE_TEMP(1);
        cfg.telemetry.enabled_signal_ids[2] = TSIG_ZONE_COOLING_STATE(0);
        cfg.telemetry.enabled_signal_ids[3] = TSIG_ACTUATOR_DUTY(0);
        cfg.telemetry.enabled_signal_ids[4] = TSIG_ACTUATOR_DUTY(1);
        cfg.telemetry.enabled_signal_ids[5] = TSIG_ACTUATOR_RPM(0);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        run_step(&ctx, 100, 55000, 1, 65000, 1, 0, 0, 2, &out);
        EXPECT_EQ(mock_tlm_count, 6);
        /* Check the two zone temps came through with the right values. */
        int saw_z0 = 0, saw_z1 = 0;
        for (uint16_t i = 0; i < mock_tlm_count; i++) {
            if (mock_tlms[i].sig == TSIG_ZONE_TEMP(0)) {
                EXPECT_EQ(mock_tlms[i].val, 55000);
                saw_z0 = 1;
            }
            if (mock_tlms[i].sig == TSIG_ZONE_TEMP(1)) {
                EXPECT_EQ(mock_tlms[i].val, 65000);
                saw_z1 = 1;
            }
        }
        EXPECT_EQ(saw_z0, 1);
        EXPECT_EQ(saw_z1, 1);
    }
}
