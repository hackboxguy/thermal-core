/* test/unit/test_runtime_cfg_load.c
 *
 * Verify the JSON loader populates a non-NULL
 * thermalcored_runtime_cfg_t with platform-only fields (PRD §5.1):
 * per-sensor / per-actuator / per-context source paths, hwmon pwm
 * and tach paths, pwm_freq_hz, tach_pulses_per_rev, telemetry
 * transport URI, control listen spec.
 *
 * Single TEST_CASE with one positive scenario (load
 * configs/minimal-1zone-1fan.json) and one negative scenario
 * (path-too-long string overflows THERMAL_PATH_MAX and the loader
 * rejects it).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "thermal_core.h"
#include "thermal_config.h"
#include "config_jsmn.h"
#include "runtime_cfg.h"

static char *read_file_or_die(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "FAIL: cannot open '%s'\n", path); exit(1); }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); exit(1); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); exit(1); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); exit(1); }
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(f); exit(1); }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); exit(1); }
    *out_len = n;
    return buf;
}

TEST_CASE(runtime_cfg_load) {
    thermal_config_t           cfg;
    thermalcored_runtime_cfg_t runtime;
    char                       err[256];

    /* === Positive: load minimal-1zone-1fan.json ============== */
    {
        size_t n;
        char *json = read_file_or_die("configs/minimal-1zone-1fan.json", &n);
        thermal_status_t s = thermal_config_jsmn_parse(json, n, &cfg, &runtime,
                                                       err, sizeof(err));
        if (s != THERMAL_OK) {
            fprintf(stderr, "load: status=%d err='%s'\n", (int)s, err);
        }
        EXPECT_STATUS_OK(s);

        /* Slot counts mirror cfg counts. */
        EXPECT_EQ(runtime.sensor_count,   cfg.sensor_count);
        EXPECT_EQ(runtime.actuator_count, cfg.actuator_count);
        EXPECT_EQ(runtime.context_count,  cfg.context_count);

        /* Per-sensor source path */
        EXPECT_EQ(strcmp(runtime.sensors[0].source,
                         "/tmp/thermal-core/sensors/soc"), 0);

        /* Per-actuator hwmon paths + pwm_freq + tach_pulses */
        EXPECT_EQ(strcmp(runtime.actuators[0].pwm,
                         "/sys/class/hwmon/hwmon0/pwm1"), 0);
        EXPECT_EQ(strcmp(runtime.actuators[0].tach,
                         "/sys/class/hwmon/hwmon0/fan1_input"), 0);
        EXPECT_EQ(runtime.actuators[0].pwm_freq_hz, 25000);
        EXPECT_EQ(runtime.actuators[0].tach_pulses_per_rev, 2);

        /* Per-context source path */
        EXPECT_EQ(strcmp(runtime.contexts[0].source,
                         "/tmp/thermal-core/contexts/vehicle_speed"), 0);

        /* Global transport + control listen.  Reference config now
         * uses the Stage 10 UDP URI per PRD §7.3 line 945; the
         * codex-v7 carryover commit also added control.enable
         * (off-by-default in the production-safe reference config). */
        EXPECT_EQ(strcmp(runtime.global.telemetry_transport,
                         "udp:127.0.0.1:9000"), 0);
        EXPECT_EQ(strcmp(runtime.global.control_listen,
                         "udp:127.0.0.1:9002"), 0);
        EXPECT_EQ((int)runtime.global.control_enable, 0);

        free(json);
    }

    /* === Negative: sensor source longer than THERMAL_PATH_MAX = */
    /* INVALID_ARG with location naming sensors[0].source.        */
    {
        char buf[512];
        size_t off = 0;
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
            "{ \"config_version\":1, \"control_period_ms\":100,"
            "  \"sensors\":[{\"id\":0,\"name\":\"soc\","
            "                \"iir_alpha_q16\":16384,"
            "                \"max_staleness_ms\":500,"
            "                \"source\":\"");
        /* THERMAL_PATH_MAX = 128, so 200 chars overflows. */
        for (int i = 0; i < 200; i++) buf[off++] = 'x';
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "\"}] }");
        thermal_status_t s = thermal_config_jsmn_parse(buf, off, &cfg,
                                                       &runtime,
                                                       err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
        if (strstr(err, "source") == NULL) {
            fprintf(stderr, "neg: err did not name 'source': '%s'\n", err);
            exit(1);
        }
    }
}
