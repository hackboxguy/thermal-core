/* test/unit/core_only_runner.c
 *
 * Stage 0 portability-guard runtime check, paired with the static nm -u
 * check in CI. Links only against core/libthermal_core.a + this file +
 * harness.h, with -Wl,--wrap=<forbidden> on the link line so any call
 * from core/ to a wrapped symbol aborts.
 *
 * Per implementation plan §5 Stage 0: "Because the binary contains no
 * platform/test code, any wrapped call is necessarily a core call —
 * no caller-attribution magic required."
 *
 * The wrap set is intentionally a small representative subset of the
 * full forbidden set; the authoritative check is the static nm -u
 * against ci/core-symbol-denylist.txt.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "thermal_core.h"

static void abort_forbidden(const char *name) {
    fprintf(stderr,
            "FATAL: core/ called forbidden symbol %s "
            "— see ci/core-symbol-denylist.txt\n",
            name);
    abort();
}

void *__wrap_malloc(size_t s)                          { (void)s;                    abort_forbidden("malloc");  return NULL; }
void *__wrap_calloc(size_t n, size_t s)                { (void)n; (void)s;           abort_forbidden("calloc");  return NULL; }
void *__wrap_realloc(void *p, size_t s)                { (void)p; (void)s;           abort_forbidden("realloc"); return NULL; }
void  __wrap_free(void *p)                             { (void)p;                    abort_forbidden("free");                 }
long  __wrap_read(int fd, void *b, size_t n)           { (void)fd; (void)b; (void)n; abort_forbidden("read");    return -1;   }
long  __wrap_write(int fd, const void *b, size_t n)    { (void)fd; (void)b; (void)n; abort_forbidden("write");   return -1;   }

static void build_pid_config(thermal_config_t *cfg) {
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
    cfg->actuators[0].state_pwm[0] = 0;
    cfg->actuators[0].state_pwm[1] = 100;
    cfg->actuators[0].state_pwm[2] = 160;
    cfg->actuators[0].state_pwm[3] = 220;
    cfg->actuators[0].state_pwm[4] = 255;
    cfg->zone_count = 1;
    cfg->zones[0].sensor_count = 1;
    cfg->zones[0].sensor_ids[0] = 0;
    cfg->zones[0].aggregation = THERMAL_AGG_MAX;
    cfg->zones[0].governor = THERMAL_GOVERNOR_PID;
    cfg->zones[0].pid.kp_q16 = 4915;
    cfg->zones[0].pid.ki_q16 = 327;
    cfg->zones[0].pid.kd_q16 = 0;
    cfg->zones[0].pid.setpoint_mc = 75000;
    cfg->zones[0].pid.kp_min_q16 = 0;
    cfg->zones[0].pid.kp_max_q16 = 327680;
    cfg->zones[0].pid.ki_min_q16 = 0;
    cfg->zones[0].pid.ki_max_q16 = 65536;
    cfg->zones[0].pid.kd_min_q16 = 0;
    cfg->zones[0].pid.kd_max_q16 = 65536;
    cfg->zones[0].pid.setpoint_min_mc = 50000;
    cfg->zones[0].pid.setpoint_max_mc = 95000;
    cfg->zones[0].pid.dt_min_ms = 50;
    cfg->zones[0].pid.dt_max_ms = 500;
    cfg->zones[0].actuator_count = 1;
    cfg->zones[0].actuator_ids[0] = 0;
    cfg->zones[0].fallback_temp_mc = 85000;
    cfg->zones[0].trip_count = 1;
    cfg->zones[0].trips[0].temp_mc = 90000;
    cfg->zones[0].trips[0].hyst_mc = 2000;
    cfg->zones[0].trips[0].severity = THERMAL_TRIP_CRITICAL;
    cfg->zones[0].trips[0].cooling_state = 3;
}

#if THERMALCORE_ENABLE_FAN_HEALTH
/* Step-wise config with the fan-health detector enabled, so the
 * portability runner exercises thermal_fan_health_step (interpolation,
 * EMA fold, aggregate) under the --wrap'd libc. */
static void build_fan_health_config(thermal_config_t *cfg) {
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
    cfg->fan_health[0].enable = 1;
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
#endif

static void build_minimal_config(thermal_config_t *cfg) {
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
}

TEST_CASE(core_only_no_forbidden_calls) {
    /* === API surface 1: validate_config(NULL) -> INVALID_ARG === */
    EXPECT_EQ(thermal_core_validate_config(NULL), THERMAL_ERR_INVALID_ARG);

    /* === API surface 2: full init + step + get_state path ===
     * Exercises the wired control loop from inside the wrap'd runner.
     * Any forbidden libc symbol reachable from this path would either
     * trip the static nm -u check or the runtime --wrap abort. */
    thermal_config_t cfg;
    build_minimal_config(&cfg);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);

    thermal_core_t ctx;
    thermal_core_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));   /* both function pointers NULL is OK */
    EXPECT_EQ(thermal_core_init(&ctx, &cfg, &cb), THERMAL_OK);

    /* Step with a sample below the trip: cooling_state should stay 0. */
    thermal_sample_t samples[1];
    samples[0].id = 0;
    samples[0].kind = THERMAL_SAMPLE_TEMP_MC;
    samples[0].valid = 1;
    samples[0].value = 65000;
    samples[0].sample_ts_ms = 0;
    samples[0].quality = 0;
    thermal_input_snapshot_t snap;
    snap.now_ms = 0;
    snap.samples = samples;
    snap.sample_count = 1;
    thermal_output_frame_t out;
    memset(&out, 0, sizeof(out));
    EXPECT_EQ(thermal_core_step(&ctx, &snap, &out), THERMAL_OK);

    thermal_state_snapshot_t state;
    EXPECT_EQ(thermal_core_get_state(&ctx, &state), THERMAL_OK);
    EXPECT_EQ(state.zones[0].temp_mc, 65000);
    EXPECT_EQ(state.zones[0].cooling_state, 0);
    EXPECT_EQ(state.zones[0].active_trip_mask, 0);

    /* Step with a sample above the trip: cooling_state -> 2. */
    samples[0].value = 75000;
    samples[0].sample_ts_ms = 100;
    snap.now_ms = 100;
    EXPECT_EQ(thermal_core_step(&ctx, &snap, &out), THERMAL_OK);
    EXPECT_EQ(thermal_core_get_state(&ctx, &state), THERMAL_OK);
    EXPECT_EQ(state.zones[0].temp_mc, 75000);
    EXPECT_EQ(state.zones[0].cooling_state, 2);
    EXPECT_EQ(state.zones[0].active_trip_mask, 0x1);

    /* === API surface 3: PID-zone init + step + get_state ===
     * Verifies the PID dispatch path uses no forbidden libc symbols.
     * Config has one critical trip at 90000 (rule 31 requires it). */
    build_pid_config(&cfg);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);
    EXPECT_EQ(thermal_core_init(&ctx, &cfg, &cb), THERMAL_OK);

    /* Temp below the critical trip: cooling_state stays 0 for PID zones. */
    samples[0].value = 80000;
    samples[0].sample_ts_ms = 0;
    snap.now_ms = 0;
    EXPECT_EQ(thermal_core_step(&ctx, &snap, &out), THERMAL_OK);
    EXPECT_EQ(thermal_core_get_state(&ctx, &state), THERMAL_OK);
    EXPECT_EQ(state.zones[0].temp_mc, 80000);
    EXPECT_EQ(state.zones[0].effective_setpoint_mc, 75000);
    EXPECT_EQ(state.zones[0].cooling_state, 0);
    EXPECT_EQ(state.zones[0].active_trip_mask, 0);

    /* Temp crosses the critical trip: cooling_state -> trip's cs (3). */
    samples[0].value = 95000;
    samples[0].sample_ts_ms = 100;
    snap.now_ms = 100;
    EXPECT_EQ(thermal_core_step(&ctx, &snap, &out), THERMAL_OK);
    EXPECT_EQ(thermal_core_get_state(&ctx, &state), THERMAL_OK);
    EXPECT_EQ(state.zones[0].temp_mc, 95000);
    EXPECT_EQ(state.zones[0].cooling_state, 3);
    EXPECT_EQ(state.zones[0].active_trip_mask, 0x1);

#if THERMALCORE_ENABLE_FAN_HEALTH
    /* === API surface 4: fan-health detector init + steady steps ===
     * Runs the fan at a steady duty with valid steady tach samples so
     * thermal_fan_health_step reaches an interpolation + EMA fold --
     * proving that path uses no forbidden libc symbol either. */
    build_fan_health_config(&cfg);
    EXPECT_EQ(thermal_core_validate_config(&cfg), THERMAL_OK);
    EXPECT_EQ(thermal_core_init(&ctx, &cfg, &cb), THERMAL_OK);

    thermal_sample_t fh_samples[2];
    fh_samples[0].id = 0;  fh_samples[0].kind = THERMAL_SAMPLE_TEMP_MC;
    fh_samples[0].valid = 1; fh_samples[0].quality = 0;
    fh_samples[1].id = 0;  fh_samples[1].kind = THERMAL_SAMPLE_TACH_RPM;
    fh_samples[1].valid = 1; fh_samples[1].value = 2000; fh_samples[1].quality = 0;
    snap.samples = fh_samples;
    snap.sample_count = 2;
    for (uint32_t t = 0; t < 8; t++) {
        uint32_t now = t * 100u;
        fh_samples[0].value = 75000;          /* above WARN -> fan runs */
        fh_samples[0].sample_ts_ms = now;
        fh_samples[1].sample_ts_ms = now;
        snap.now_ms = now;
        EXPECT_EQ(thermal_core_step(&ctx, &snap, &out), THERMAL_OK);
    }
#endif
}
