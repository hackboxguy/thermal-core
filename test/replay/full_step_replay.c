/* test/replay/full_step_replay.c
 *
 * Stage 7 commit 7d full-step replay golden driver. Runs
 * thermal_core_step() through a 60-second (600-tick @ 100ms) scenario
 * that exercises every wired path: cold start, WARN under acoustic
 * cap, speed ramp relaxing cap, CRITICAL bypass + override, recovery
 * downward slew, runaway, SHUTDOWN. Emits one CSV row per tick from
 * the output frame + thermal_core_get_state() snapshot.
 *
 * The CSV is the canonical "did anything change anywhere in the loop"
 * canary. Any wired-path regression that affects the captured columns
 * will be caught by diff against test/replay/golden/full_step_sweep.csv.
 *
 * Per-module Python references already gate cross-implementation
 * portability for each module's math; this golden is C-only by design
 * (impl-plan §2.3 + user scope decision).
 *
 * No Python reference companion.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "thermal_core.h"
#include "thermal_config.h"
#include "thermal_types.h"

#define N_TICKS     600
#define TICK_MS     100
#define SENSOR_ID   1
#define CONTEXT_ID  100
#define ACTUATOR_ID 10

/* ------------------------------------------------------------------ */
/* Scenario profile: temperature and speed as functions of tick.       */
/* See plan: 6 phases, each 100 ticks (10 seconds).                    */
/* ------------------------------------------------------------------ */
static int32_t synth_temp(int t) {
    if (t < 100) {
        /* Phase 1: cold start, 40°C flat. */
        return 40000;
    } else if (t < 200) {
        /* Phase 2: linear ramp 40 -> 75°C over 100 ticks. */
        int x = t - 100;
        return 40000 + (x * 35000) / 100;
    } else if (t < 300) {
        /* Phase 3: hold at 75°C while speed ramps. */
        return 75000;
    } else if (t < 400) {
        /* Phase 4: jump to 90°C (5 ticks ramp) then hold. */
        int x = t - 300;
        if (x < 5) {
            return 75000 + (x * 15000) / 5;
        }
        return 90000;
    } else if (t < 500) {
        /* Phase 5: linear drop 90 -> 50°C over 100 ticks. */
        int x = t - 400;
        return 90000 - (x * 40000) / 100;
    } else {
        /* Phase 6: linear climb 50 -> 100°C over 100 ticks. */
        int x = t - 500;
        return 50000 + (x * 50000) / 100;
    }
}

static int32_t synth_speed(int t) {
    if (t < 200) {
        /* Phases 1-2: stationary. */
        return 0;
    } else if (t < 300) {
        /* Phase 3: linear ramp 0 -> 100 km/h. */
        int x = t - 200;
        return (x * 100) / 100;
    }
    /* Phases 4-6: hold 100 km/h. */
    return 100;
}

/* ------------------------------------------------------------------ */
/* Config builder.                                                     */
/* ------------------------------------------------------------------ */
static void build_cfg(thermal_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->config_version = 1;
    cfg->control_period_ms = TICK_MS;

    cfg->sensor_count = 1;
    cfg->sensors[0].id = SENSOR_ID;
    cfg->sensors[0].iir_alpha_q16 = Q16_ONE;   /* pass-through */
    cfg->sensors[0].max_staleness_ms = 1000;

    cfg->context_count = 1;
    cfg->contexts[0].id = CONTEXT_ID;
    cfg->contexts[0].unit = THERMAL_CONTEXT_UNIT_KMH;
    cfg->contexts[0].iir_alpha_q16 = Q16_ONE;   /* pass-through */
    cfg->contexts[0].timeout_ms = 500;
    cfg->contexts[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;

    cfg->actuator_count = 1;
    cfg->actuators[0].id = ACTUATOR_ID;
    cfg->actuators[0].pwm_min = 80;
    cfg->actuators[0].pwm_max = 255;
    cfg->actuators[0].slew_per_tick = 8;
    cfg->actuators[0].spinup_pwm = 200;
    cfg->actuators[0].spinup_ms = 300;
    cfg->actuators[0].state_pwm[0] = 0;
    cfg->actuators[0].state_pwm[1] = 100;
    cfg->actuators[0].state_pwm[2] = 160;
    cfg->actuators[0].state_pwm[3] = 220;
    cfg->actuators[0].state_pwm[4] = 255;

    cfg->zone_count = 1;
    cfg->zones[0].sensor_count = 1;
    cfg->zones[0].sensor_ids[0] = SENSOR_ID;
    cfg->zones[0].aggregation = THERMAL_AGG_MAX;
    cfg->zones[0].governor = THERMAL_GOVERNOR_STEP_WISE;
    cfg->zones[0].actuator_count = 1;
    cfg->zones[0].actuator_ids[0] = ACTUATOR_ID;
    cfg->zones[0].fallback_temp_mc = 85000;
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

    cfg->modifier_count = 1;
    strncpy(cfg->modifiers[0].name, "acoustic_mask", THERMAL_NAME_MAX - 1);
    cfg->modifiers[0].context_id = CONTEXT_ID;
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

    /* Runaway detector enabled (other three detectors disabled in this
     * scenario; their state machines are covered by per-detector
     * goldens). Runaway fires when temp climbs while commanded PWM is
     * high. */
    cfg->faults.runaway_defaults.enabled = 1;
    cfg->faults.runaway_defaults.severity = THERMAL_FAULT_SEVERITY_CRITICAL;
    cfg->faults.runaway_defaults.action =
        THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED;
    cfg->faults.runaway_defaults.persist_ticks = 20;
    cfg->faults.runaway_defaults.recovery_ticks = 20;
    cfg->faults.runaway_defaults.threshold0 = 5000; /* rise_mc over window */
    cfg->faults.runaway_defaults.threshold1 = 200;  /* cooling_pwm threshold */
}

int main(void) {
    thermal_config_t cfg;
    build_cfg(&cfg);

    thermal_core_t ctx;
    thermal_core_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));   /* NULL callbacks — core only checks ptrs */

    if (thermal_core_init(&ctx, &cfg, &cb) != THERMAL_OK) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    printf("tick,now_ms,zone0_temp_mc,zone0_trip_mask,zone0_cooling_state,"
           "act0_duty,act0_requested,act0_slew_limited,act0_reason,"
           "mod0_active,mod0_pwm_cap,mod0_trip_offset_mc,fault_count\n");

    thermal_sample_t samples[2];
    thermal_input_snapshot_t snap;
    thermal_output_frame_t out;
    thermal_state_snapshot_t state;

    for (int t = 0; t < N_TICKS; t++) {
        samples[0].id = SENSOR_ID;
        samples[0].kind = THERMAL_SAMPLE_TEMP_MC;
        samples[0].valid = 1;
        samples[0].value = synth_temp(t);
        samples[0].sample_ts_ms = (uint32_t)(t * TICK_MS);
        samples[0].quality = 0;
        samples[1].id = CONTEXT_ID;
        samples[1].kind = THERMAL_SAMPLE_CONTEXT_I32;
        samples[1].valid = 1;
        samples[1].value = synth_speed(t);
        samples[1].sample_ts_ms = (uint32_t)(t * TICK_MS);
        samples[1].quality = 0;
        snap.now_ms = (uint32_t)(t * TICK_MS);
        snap.samples = samples;
        snap.sample_count = 2;

        if (thermal_core_step(&ctx, &snap, &out) != THERMAL_OK) {
            fprintf(stderr, "step failed at tick %d\n", t);
            return 1;
        }
        if (thermal_core_get_state(&ctx, &state) != THERMAL_OK) {
            fprintf(stderr, "get_state failed at tick %d\n", t);
            return 1;
        }

        printf("%d,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u\n",
               t,
               (unsigned)(t * TICK_MS),
               state.zones[0].temp_mc,
               (unsigned)state.zones[0].active_trip_mask,
               (unsigned)state.zones[0].cooling_state,
               (unsigned)state.actuators[0].duty_0_255,
               (unsigned)state.actuators[0].requested_duty_0_255,
               (unsigned)state.actuators[0].slew_limited,
               (unsigned)state.actuators[0].reason,
               (unsigned)state.modifiers[0].active,
               (unsigned)state.modifiers[0].pwm_cap_0_255,
               state.modifiers[0].trip_offset_mc,
               (unsigned)state.fault_count);
    }

    return 0;
}
