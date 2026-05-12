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
     * Temp 86°C -> CRITICAL active. Modifier configured to cap at 120
     * (speed=0). PRD §4.6 line 598 -> cap is skipped. safety_override
     * is also set -> duty = pwm_max=255.
     * ============================================================ */
    {
        build_base_cfg(&cfg);
        add_acoustic_mask(&cfg);
        mock_reset();
        EXPECT_STATUS_OK(thermal_core_init(&ctx, &cfg, &MOCK_CB));
        step_t1c(&ctx, 100, 86000, 0, &out);
        /* Speed=0 means modifier wants pwm_cap=120. But CRITICAL active
         * for this actuator's driving zone -> cap skipped + override-up
         * -> slew bypass -> immediately pwm_max=255 on tick 1. */
        EXPECT_EQ(out.actuator_cmds[0].duty_0_255, 255);
        EXPECT_EQ(out.actuator_cmds[0].reason, THERMAL_ACT_REASON_FAULT_RUNAWAY);
        /* TEVENT_SAFETY_OVERRIDE emitted on rising edge of the override. */
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
}
