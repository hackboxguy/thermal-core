/* test/parity/replay_run.c -- see replay_run.h for the contract. */
#include "replay_run.h"

#include <stdio.h>
#include <string.h>

#include "canonical.h"
#include "replay_fixture.h"
#include "thermal_config.h"
#include "thermal_types.h"

void replay_run(thermal_core_t *core, const thermal_config_t *cfg)
{
    fputs(THERMALCORE_CANONICAL_HEADER, stdout);

    const uint16_t dt_ms = cfg->control_period_ms;

    for (uint16_t tick = 0; tick < G_REPLAY_TICK_COUNT; tick++) {
        uint32_t      now_ms = (uint32_t)tick * (uint32_t)dt_ms;
        replay_tick_t rt     = replay_fixture_tick(tick);

        thermal_sample_t samples[THERMAL_MAX_SAMPLES_PER_SNAPSHOT];
        uint8_t          n = 0;

        for (uint8_t i = 0; i < cfg->sensor_count; i++) {
            samples[n].id           = cfg->sensors[i].id;
            samples[n].kind         = THERMAL_SAMPLE_TEMP_MC;
            samples[n].sample_ts_ms = now_ms;
            samples[n].value        = rt.temp_mc;
            samples[n].valid        = 1;
            samples[n].quality      = 0;
            n++;
        }
        for (uint8_t i = 0; i < cfg->actuator_count; i++) {
            samples[n].id           = cfg->actuators[i].id;
            samples[n].kind         = THERMAL_SAMPLE_TACH_RPM;
            samples[n].sample_ts_ms = now_ms;
            samples[n].value        = (int32_t)rt.tach_rpm;
            samples[n].valid        = 1;
            samples[n].quality      = 0;
            n++;
        }

        thermal_input_snapshot_t snap = {
            .now_ms       = now_ms,
            .samples      = samples,
            .sample_count = n,
        };
        thermal_output_frame_t out;
        memset(&out, 0, sizeof(out));
        (void)thermal_core_step(core, &snap, &out);
    }

    fputs("END\n", stdout);
}
