/* test/unit/test_config_jsmn.c
 *
 * Unit tests for the Stage 9 9a JSON config loader
 * (platform/linux/config_jsmn.{c,h}).  One TEST_CASE with twelve
 * labeled scenarios:
 *
 *   1.  Positive minimal load (configs/minimal-1zone-1fan.json).
 *   2.  Missing top-level config_version.
 *   3.  Unsupported config_version (= 7) — validate_config rejects.
 *   4.  Wrong type for a numeric field.
 *   5.  Unknown enum.
 *   6.  Zone references an unknown sensor.
 *   7.  Too many sensors (9 with THERMAL_MAX_SENSORS = 8).
 *   8.  Malformed JSON (unbalanced brace).
 *   9.  Missing required sensor field.
 *   10. Unknown CORE field on a sensor object.
 *   11. Telemetry wildcard expansion ("zone_temp_*").
 *   12. Telemetry wildcard expands to nothing ("actuator_rpm_*"
 *       with actuator_count = 0).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "thermal_core.h"
#include "thermal_config.h"
#include "thermal_signals.h"
#include "config_jsmn.h"

/* === File loader for scenario 1 ============================== */

/* Read the file at path into a malloc'd buffer; *out_len receives
 * the byte count. Exits the test on any error. */
static char *read_file_or_die(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open '%s'\n", path);
        exit(1);
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); exit(1); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); exit(1); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); exit(1); }

    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(f); exit(1); }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        fprintf(stderr, "FAIL: short read of '%s'\n", path);
        free(buf);
        exit(1);
    }
    *out_len = n;
    return buf;
}

TEST_CASE(config_jsmn) {
    thermal_config_t cfg;
    char err[256];

    /* === Scenario 1: positive minimal load ==================== */
    {
        size_t n;
        char *json = read_file_or_die("configs/minimal-1zone-1fan.json", &n);
        thermal_status_t s = thermal_config_jsmn_parse(json, n, &cfg,
                                                       err, sizeof(err));
        if (s != THERMAL_OK) {
            fprintf(stderr, "scenario 1: status=%d err='%s'\n", (int)s, err);
        }
        EXPECT_STATUS_OK(s);

        EXPECT_EQ(cfg.config_version, 1);
        EXPECT_EQ(cfg.control_period_ms, 100);
        EXPECT_EQ(cfg.sensor_count, 1);
        EXPECT_EQ(cfg.actuator_count, 1);
        EXPECT_EQ(cfg.context_count, 1);
        EXPECT_EQ(cfg.zone_count, 1);
        EXPECT_EQ(cfg.modifier_count, 1);

        /* Sensor: name + filter alpha */
        EXPECT_EQ(strcmp(cfg.sensors[0].name, "soc"), 0);
        EXPECT_EQ(cfg.sensors[0].iir_alpha_q16, 16384);
        EXPECT_EQ(cfg.sensors[0].max_staleness_ms, 500);

        /* Actuator state_pwm array */
        EXPECT_EQ(cfg.actuators[0].pwm_min, 80);
        EXPECT_EQ(cfg.actuators[0].pwm_max, 255);
        EXPECT_EQ(cfg.actuators[0].state_pwm[0], 0);
        EXPECT_EQ(cfg.actuators[0].state_pwm[4], 255);

        /* Zone trip 0 — temp_mc and severity decoded correctly */
        EXPECT_EQ(cfg.zones[0].trip_count, 2);
        EXPECT_EQ(cfg.zones[0].trips[0].temp_mc, 70000);
        EXPECT_EQ(cfg.zones[0].trips[0].severity, THERMAL_TRIP_WARN);
        EXPECT_EQ(cfg.zones[0].trips[1].severity, THERMAL_TRIP_CRITICAL);

        /* Modifier resolves "vehicle_speed" -> context id 0. */
        EXPECT_EQ(cfg.modifiers[0].context_id, 0);
        EXPECT_EQ(cfg.modifiers[0].stages,
                  (uint8_t)THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP);
        EXPECT_EQ(cfg.modifiers[0].curve_count, 2);

        /* Telemetry: zone_temp_* (1) + actuator_pwm_* (1) + speed_kmh (1). */
        EXPECT_EQ(cfg.telemetry.enabled_signal_count, 3);
        EXPECT_EQ(cfg.telemetry.enabled_signal_ids[0], TSIG_ZONE_TEMP(0));
        EXPECT_EQ(cfg.telemetry.enabled_signal_ids[1], TSIG_ACTUATOR_DUTY(0));
        EXPECT_EQ(cfg.telemetry.enabled_signal_ids[2], TSIG_CONTEXT_VALUE(0));

        free(json);
    }

    /* === Scenario 2: missing config_version =================== */
    {
        const char *json =
            "{ \"control_period_ms\": 100,"
            "  \"sensors\": [{\"id\":0, \"name\":\"soc\","
            "                 \"iir_alpha_q16\":16384,"
            "                 \"max_staleness_ms\":500}] }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
    }

    /* === Scenario 3: unsupported config_version (= 7) ========= */
    {
        const char *json =
            "{ \"config_version\": 7,"
            "  \"control_period_ms\": 100 }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, err, sizeof(err));
        /* validate_config rule 2 — wrong config_version */
        EXPECT_EQ(s, THERMAL_ERR_INVALID_CONFIG);
    }

    /* === Scenario 4: wrong type for control_period_ms ========= */
    {
        const char *json =
            "{ \"config_version\": 1,"
            "  \"control_period_ms\": \"100\" }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
    }

    /* === Scenario 5: unknown enum value ======================= */
    {
        const char *json =
            "{ \"config_version\": 1,"
            "  \"control_period_ms\": 100,"
            "  \"sensors\": [{\"id\":0, \"name\":\"soc\","
            "                 \"iir_alpha_q16\":16384,"
            "                 \"max_staleness_ms\":500}],"
            "  \"actuators\": [{\"id\":0, \"name\":\"f\","
            "                   \"pwm_min\":80, \"pwm_max\":255,"
            "                   \"state_pwm\":[0,100,160,220,255]}],"
            "  \"zones\": [{ \"name\":\"z\","
            "                \"sensors\":[\"soc\"],"
            "                \"aggregation\":\"median\","   /* unknown */
            "                \"fallback_temp_mc\":85000,"
            "                \"governor\":\"step_wise\","
            "                \"actuators\":[\"f\"],"
            "                \"trips\":[{\"temp_mc\":70000,\"hyst_mc\":2000,"
            "                            \"severity\":\"warn\",\"cooling_state\":1}] }] }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
    }

    /* === Scenario 6: zone references unknown sensor =========== */
    {
        const char *json =
            "{ \"config_version\": 1,"
            "  \"control_period_ms\": 100,"
            "  \"sensors\": [{\"id\":0, \"name\":\"soc\","
            "                 \"iir_alpha_q16\":16384,"
            "                 \"max_staleness_ms\":500}],"
            "  \"actuators\": [{\"id\":0, \"name\":\"f\","
            "                   \"pwm_min\":80, \"pwm_max\":255,"
            "                   \"state_pwm\":[0,100,160,220,255]}],"
            "  \"zones\": [{ \"name\":\"z\","
            "                \"sensors\":[\"nonexistent\"],"     /* bad ref */
            "                \"aggregation\":\"max\","
            "                \"fallback_temp_mc\":85000,"
            "                \"governor\":\"step_wise\","
            "                \"actuators\":[\"f\"],"
            "                \"trips\":[{\"temp_mc\":70000,\"hyst_mc\":2000,"
            "                            \"severity\":\"warn\",\"cooling_state\":1}] }] }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
        /* Error message should name the missing reference. */
        if (strstr(err, "nonexistent") == NULL) {
            fprintf(stderr, "scenario 6: err did not name bad ref: '%s'\n", err);
            exit(1);
        }
    }

    /* === Scenario 7: too many sensors (9 with MAX = 8) ======== */
    {
        /* Build the 9-sensor JSON dynamically. */
        char buf[2048];
        size_t off = 0;
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
            "{ \"config_version\":1, \"control_period_ms\":100, \"sensors\":[");
        for (int i = 0; i < THERMAL_MAX_SENSORS + 1; i++) {
            off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                "%s{\"id\":%d,\"name\":\"s%d\","
                "\"iir_alpha_q16\":16384,\"max_staleness_ms\":500}",
                (i == 0) ? "" : ",", i, i);
        }
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "] }");
        thermal_status_t s = thermal_config_jsmn_parse(buf, off, &cfg,
                                                       err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_NO_SPACE);
    }

    /* === Scenario 8: malformed JSON (unbalanced brace) ======== */
    {
        const char *json =
            "{ \"config_version\": 1, \"control_period_ms\": 100 ";  /* no '}' */
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
    }

    /* === Scenario 9: missing required sensor field (no id) === */
    {
        const char *json =
            "{ \"config_version\": 1,"
            "  \"control_period_ms\": 100,"
            "  \"sensors\": [{\"name\":\"soc\","
            "                 \"iir_alpha_q16\":16384,"
            "                 \"max_staleness_ms\":500}] }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
    }

    /* === Scenario 10: unknown CORE field on a sensor ========== */
    {
        const char *json =
            "{ \"config_version\": 1,"
            "  \"control_period_ms\": 100,"
            "  \"sensors\": [{\"id\":0,\"name\":\"soc\","
            "                 \"iir_alpha_q16\":16384,"
            "                 \"max_staleness_ms\":500,"
            "                 \"garbage\":1}] }";       /* unknown */
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
        if (strstr(err, "garbage") == NULL) {
            fprintf(stderr, "scenario 10: err did not name bad key: '%s'\n", err);
            exit(1);
        }
    }

    /* === Scenario 11: telemetry wildcard expansion ============ */
    {
        /* Minimal valid config + "zone_temp_*" wildcard. */
        const char *json =
            "{ \"config_version\":1, \"control_period_ms\":100,"
            "  \"sensors\":[{\"id\":0,\"name\":\"soc\","
            "                \"iir_alpha_q16\":16384,"
            "                \"max_staleness_ms\":500}],"
            "  \"actuators\":[{\"id\":0,\"name\":\"f\","
            "                  \"pwm_min\":80,\"pwm_max\":255,"
            "                  \"state_pwm\":[0,100,160,220,255]}],"
            "  \"zones\":[{ \"name\":\"z0\","
            "               \"sensors\":[\"soc\"],"
            "               \"aggregation\":\"max\","
            "               \"fallback_temp_mc\":85000,"
            "               \"governor\":\"step_wise\","
            "               \"actuators\":[\"f\"],"
            "               \"trips\":[{\"temp_mc\":70000,\"hyst_mc\":2000,"
            "                           \"severity\":\"warn\",\"cooling_state\":1}] }],"
            "  \"telemetry\":{ \"enable\":true, \"period_ticks\":10,"
            "                  \"signals\":[\"zone_temp_*\"] } }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, err, sizeof(err));
        if (s != THERMAL_OK) {
            fprintf(stderr, "scenario 11: status=%d err='%s'\n", (int)s, err);
        }
        EXPECT_STATUS_OK(s);
        EXPECT_EQ(cfg.telemetry.enabled_signal_count, cfg.zone_count);
        EXPECT_EQ(cfg.telemetry.enabled_signal_ids[0], TSIG_ZONE_TEMP(0));
    }

    /* === Scenario 12: wildcard expands to nothing ============= */
    {
        /* actuator_count = 0 -> "actuator_rpm_*" expands to nothing
         * which the loader treats as an error (cannot tell user-intent
         * vs no-op from the JSON). */
        const char *json =
            "{ \"config_version\":1, \"control_period_ms\":100,"
            "  \"telemetry\":{ \"enable\":true, \"period_ticks\":10,"
            "                  \"signals\":[\"actuator_rpm_*\"] } }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
    }
}
