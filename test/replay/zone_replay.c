/* test/replay/zone_replay.c
 *
 * Stage 4 heat-soak module golden driver. Builds a 1-sensor 1-actuator
 * 1-zone config with 3 trips (60/75/85 mc with 2 mc hyst, cooling_states
 * 1/2/3), runs thermal_core_step across 600 ticks of a synthetic
 * 45000 -> 90000 -> 45000 mc triangle wave (150 mc/tick), and emits per-
 * tick zone state to stdout via thermal_core_get_state.
 *
 * Filter alpha = Q16_ONE (passthrough) so the golden tests aggregation +
 * step-wise governor + the wiring, not IIR drift (filter_sweep covers
 * IIR behavior separately).
 *
 * Reference: test/reference/zone.py — same generator, same math, must
 * produce byte-equal CSV output.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "thermal_core.h"
#include "thermal_config.h"
#include "thermal_types.h"

#define N_TICKS    600
#define TICK_MS    100

/* Triangle wave 45000mc -> 90000mc -> 45000mc over 600 ticks. */
static int32_t synth_temp(int tick) {
    if (tick <= 300) return 45000 + (int32_t)tick * 150;
    return 90000 - (int32_t)(tick - 300) * 150;
}

int main(void) {
    thermal_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.config_version = 1;
    cfg.control_period_ms = TICK_MS;

    cfg.sensor_count = 1;
    cfg.sensors[0].id = 0;
    cfg.sensors[0].iir_alpha_q16 = Q16_ONE;
    cfg.sensors[0].max_staleness_ms = 500;

    cfg.actuator_count = 1;
    cfg.actuators[0].id = 0;
    cfg.actuators[0].pwm_min = 80;
    cfg.actuators[0].pwm_max = 255;
    cfg.actuators[0].state_pwm[0] = 0;
    cfg.actuators[0].state_pwm[1] = 100;
    cfg.actuators[0].state_pwm[2] = 160;
    cfg.actuators[0].state_pwm[3] = 220;
    cfg.actuators[0].state_pwm[4] = 255;

    cfg.zone_count = 1;
    cfg.zones[0].sensor_count = 1;
    cfg.zones[0].sensor_ids[0] = 0;
    cfg.zones[0].aggregation = THERMAL_AGG_MAX;
    cfg.zones[0].governor = THERMAL_GOVERNOR_STEP_WISE;
    cfg.zones[0].actuator_count = 1;
    cfg.zones[0].actuator_ids[0] = 0;
    cfg.zones[0].fallback_temp_mc = 85000;
    cfg.zones[0].trip_count = 3;
    cfg.zones[0].trips[0].temp_mc = 60000;
    cfg.zones[0].trips[0].hyst_mc = 2000;
    cfg.zones[0].trips[0].severity = THERMAL_TRIP_WARN;
    cfg.zones[0].trips[0].cooling_state = 1;
    cfg.zones[0].trips[1].temp_mc = 75000;
    cfg.zones[0].trips[1].hyst_mc = 2000;
    cfg.zones[0].trips[1].severity = THERMAL_TRIP_WARN;
    cfg.zones[0].trips[1].cooling_state = 2;
    cfg.zones[0].trips[2].temp_mc = 85000;
    cfg.zones[0].trips[2].hyst_mc = 2000;
    cfg.zones[0].trips[2].severity = THERMAL_TRIP_CRITICAL;
    cfg.zones[0].trips[2].cooling_state = 3;

    thermal_core_t ctx;
    thermal_core_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));   /* both function pointers NULL — allowed */

    if (thermal_core_init(&ctx, &cfg, &cb) != THERMAL_OK) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    printf("tick,now_ms,zone_temp,active_trip_mask,cooling_state\n");
    for (int t = 0; t < N_TICKS; t++) {
        uint32_t now_ms = (uint32_t)(t * TICK_MS);
        int32_t temp = synth_temp(t);
        thermal_sample_t samples[1];
        samples[0].id = 0;
        samples[0].kind = THERMAL_SAMPLE_TEMP_MC;
        samples[0].valid = 1;
        samples[0].value = temp;
        samples[0].sample_ts_ms = now_ms;
        samples[0].quality = 0;

        thermal_input_snapshot_t snap;
        snap.now_ms = now_ms;
        snap.samples = samples;
        snap.sample_count = 1;

        thermal_output_frame_t out;
        memset(&out, 0, sizeof(out));
        if (thermal_core_step(&ctx, &snap, &out) != THERMAL_OK) {
            fprintf(stderr, "step %d failed\n", t);
            return 1;
        }

        thermal_state_snapshot_t state;
        if (thermal_core_get_state(&ctx, &state) != THERMAL_OK) {
            fprintf(stderr, "get_state %d failed\n", t);
            return 1;
        }

        printf("%d,%u,%d,%u,%u\n",
               t,
               (unsigned)state.now_ms,
               state.zones[0].temp_mc,
               (unsigned)state.zones[0].active_trip_mask,
               (unsigned)state.zones[0].cooling_state);
    }
    return 0;
}
