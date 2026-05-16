/* test/parity/replay_host.c
 *
 * Host side of the Stage 15 cross-platform parity test.  Runs the
 * shared replay fixture through thermal_core_step() on the Linux
 * host and emits the canonical telemetry CSV on stdout.
 *
 * The ESP32-C3 REPLAY firmware feeds the *same* fixture through
 * the *same* core via the *same* replay_run() loop; replay-parity
 * (Stage 15c) SHA-256-compares the two byte streams.
 *
 * The config is the json2static-generated G_THERMAL_CFG from
 * platform/esp32_idf/configs/esp32-c3-standalone.json -- the
 * identical const struct the ESP32 build compiles, so both rigs
 * run byte-identical configuration.
 */
#include <stdint.h>
#include <stdio.h>

#include "canonical.h"
#include "replay_run.h"
#include "thermal_core.h"
#include "thermal_platform.h"

extern const thermal_config_t G_THERMAL_CFG;

static void host_telemetry_emit_cb(uint32_t ts_ms, uint16_t signal_id,
                                   int32_t value)
{
    char buf[128];
    int  n = thermalcore_canonical_sample(buf, sizeof buf,
                                          ts_ms, signal_id, value, 0);
    if (n > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
    }
}

static void host_log_event_cb(uint32_t ts_ms, uint16_t code,
                              uint32_t a1, uint32_t a2,
                              uint32_t a3, uint32_t a4)
{
    char buf[128];
    int  n = thermalcore_canonical_event(buf, sizeof buf,
                                         ts_ms, code, a1, a2, a3, a4);
    if (n > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
    }
}

int main(void)
{
    static thermal_core_t core;
    thermal_core_callbacks_t cb = {
        .log_event      = host_log_event_cb,
        .telemetry_emit = host_telemetry_emit_cb,
    };

    if (thermal_core_init(&core, &G_THERMAL_CFG, &cb) != THERMAL_OK) {
        fprintf(stderr, "replay_host: thermal_core_init failed\n");
        return 1;
    }

    replay_run(&core, &G_THERMAL_CFG);
    return 0;
}
