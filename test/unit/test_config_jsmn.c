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
        thermal_status_t s = thermal_config_jsmn_parse(json, n, &cfg, NULL,
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
                                                       &cfg, NULL, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
    }

    /* === Scenario 3: unsupported config_version (= 7) ========= */
    {
        const char *json =
            "{ \"config_version\": 7,"
            "  \"control_period_ms\": 100 }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, NULL, err, sizeof(err));
        /* validate_config rule 2 — wrong config_version */
        EXPECT_EQ(s, THERMAL_ERR_INVALID_CONFIG);
    }

    /* === Scenario 4: wrong type for control_period_ms ========= */
    {
        const char *json =
            "{ \"config_version\": 1,"
            "  \"control_period_ms\": \"100\" }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, NULL, err, sizeof(err));
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
                                                       &cfg, NULL, err, sizeof(err));
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
                                                       &cfg, NULL, err, sizeof(err));
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
        thermal_status_t s = thermal_config_jsmn_parse(buf, off, &cfg, NULL,
                                                       err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_NO_SPACE);
    }

    /* === Scenario 8: malformed JSON (unbalanced brace) ======== */
    {
        const char *json =
            "{ \"config_version\": 1, \"control_period_ms\": 100 ";  /* no '}' */
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, NULL, err, sizeof(err));
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
                                                       &cfg, NULL, err, sizeof(err));
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
                                                       &cfg, NULL, err, sizeof(err));
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
                                                       &cfg, NULL, err, sizeof(err));
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
                                                       &cfg, NULL, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
    }

    /* === Scenario 13: PRD fault_detection schema round-trip ===== */
    {
        /* Minimal valid config + a stall detector using the PRD's
         * descriptive thresholds (stall_rpm -> threshold0,
         * stall_pwm_threshold -> threshold1).  PRD §5 line 611. */
        const char *json =
            "{ \"config_version\":1, \"control_period_ms\":100,"
            "  \"sensors\":[{\"id\":0,\"name\":\"soc\","
            "                \"iir_alpha_q16\":16384,"
            "                \"max_staleness_ms\":500}],"
            "  \"actuators\":[{\"id\":0,\"name\":\"f\","
            "                  \"pwm_min\":80,\"pwm_max\":255,"
            "                  \"state_pwm\":[0,100,160,220,255]}],"
            "  \"zones\":[{ \"name\":\"z\","
            "               \"sensors\":[\"soc\"],"
            "               \"aggregation\":\"max\","
            "               \"fallback_temp_mc\":85000,"
            "               \"governor\":\"step_wise\","
            "               \"actuators\":[\"f\"],"
            "               \"trips\":[{\"temp_mc\":70000,\"hyst_mc\":2000,"
            "                           \"severity\":\"warn\",\"cooling_state\":1}] }],"
            "  \"fault_detection\":{ \"stall\":{"
            "       \"enabled\":true, \"severity\":\"critical\","
            "       \"action\":\"force_pwm_max_until_recovered\","
            "       \"persist_ticks\":5, \"recovery_ticks\":10,"
            "       \"stall_rpm\":200, \"stall_pwm_threshold\":80 } } }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, NULL, err, sizeof(err));
        if (s != THERMAL_OK) {
            fprintf(stderr, "scenario 13: status=%d err='%s'\n", (int)s, err);
        }
        EXPECT_STATUS_OK(s);
        EXPECT_EQ(cfg.faults.stall_defaults.threshold0, 200);
        EXPECT_EQ(cfg.faults.stall_defaults.threshold1, 80);
        /* Non-stuck_sensor detector → correlated_context_id == 0xFFFF
         * by convention (PRD §4.7 line 617). */
        EXPECT_EQ(cfg.faults.stall_defaults.correlated_context_id, 0xFFFFu);
    }

    /* === Scenario 14: stuck_sensor correlated_context: null ===== */
    {
        /* Same minimal config + stuck_sensor with null
         * correlated_context → advisory mode (0xFFFF). */
        const char *json =
            "{ \"config_version\":1, \"control_period_ms\":100,"
            "  \"sensors\":[{\"id\":0,\"name\":\"soc\","
            "                \"iir_alpha_q16\":16384,"
            "                \"max_staleness_ms\":500}],"
            "  \"actuators\":[{\"id\":0,\"name\":\"f\","
            "                  \"pwm_min\":80,\"pwm_max\":255,"
            "                  \"state_pwm\":[0,100,160,220,255]}],"
            "  \"zones\":[{ \"name\":\"z\","
            "               \"sensors\":[\"soc\"],"
            "               \"aggregation\":\"max\","
            "               \"fallback_temp_mc\":85000,"
            "               \"governor\":\"step_wise\","
            "               \"actuators\":[\"f\"],"
            "               \"trips\":[{\"temp_mc\":70000,\"hyst_mc\":2000,"
            "                           \"severity\":\"warn\",\"cooling_state\":1}] }],"
            "  \"fault_detection\":{ \"stuck_sensor\":{"
            "       \"enabled\":true, \"severity\":\"degraded\","
            "       \"action\":\"use_zone_fallback\","
            "       \"persist_ticks\":5, \"recovery_ticks\":10,"
            "       \"delta_mc\":100, \"window_ticks\":600,"
            "       \"correlated_context\": null } } }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, NULL, err, sizeof(err));
        if (s != THERMAL_OK) {
            fprintf(stderr, "scenario 14: status=%d err='%s'\n", (int)s, err);
        }
        EXPECT_STATUS_OK(s);
        EXPECT_EQ(cfg.faults.stuck_sensor_defaults.threshold0, 100);
        EXPECT_EQ(cfg.faults.stuck_sensor_defaults.threshold1, 600);
        EXPECT_EQ(cfg.faults.stuck_sensor_defaults.correlated_context_id, 0xFFFFu);
    }

    /* === Scenario 15: legacy `faults` key is now rejected ======= */
    {
        const char *json =
            "{ \"config_version\":1, \"control_period_ms\":100,"
            "  \"faults\":{} }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, NULL, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
        if (strstr(err, "faults") == NULL) {
            fprintf(stderr, "scenario 15: err did not name 'faults': '%s'\n", err);
            exit(1);
        }
    }

    /* === Scenario 16: trip cooling_state past state_pwm length === */
    {
        /* state_pwm has 2 entries (cooling_state 0..1 only), but trip
         * references cooling_state=3.  Old behavior would silently map
         * to the tail-zero-fill; the fix rejects this. */
        const char *json =
            "{ \"config_version\":1, \"control_period_ms\":100,"
            "  \"sensors\":[{\"id\":0,\"name\":\"soc\","
            "                \"iir_alpha_q16\":16384,"
            "                \"max_staleness_ms\":500}],"
            "  \"actuators\":[{\"id\":0,\"name\":\"f\","
            "                  \"pwm_min\":80,\"pwm_max\":255,"
            "                  \"state_pwm\":[0, 100]}],"
            "  \"zones\":[{ \"name\":\"z\","
            "               \"sensors\":[\"soc\"],"
            "               \"aggregation\":\"max\","
            "               \"fallback_temp_mc\":85000,"
            "               \"governor\":\"step_wise\","
            "               \"actuators\":[\"f\"],"
            "               \"trips\":[{\"temp_mc\":70000,\"hyst_mc\":2000,"
            "                           \"severity\":\"warn\","
            "                           \"cooling_state\":3}] }] }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, NULL, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
        if (strstr(err, "cooling_state") == NULL) {
            fprintf(stderr,
                    "scenario 16: err did not name cooling_state: '%s'\n", err);
            exit(1);
        }
    }

#if THERMAL_MAX_SENSORS >= 2  /* 2-sensor config; skipped under a maxima=1 profile */
    /* === Scenario 17: weighted aggregation needs N weights ====== */
    {
        /* Two sensors, weighted aggregation, but only one weight.
         * Old behavior silently zero-filled the second weight. */
        const char *json =
            "{ \"config_version\":1, \"control_period_ms\":100,"
            "  \"sensors\":["
            "    {\"id\":0,\"name\":\"a\",\"iir_alpha_q16\":16384,\"max_staleness_ms\":500},"
            "    {\"id\":1,\"name\":\"b\",\"iir_alpha_q16\":16384,\"max_staleness_ms\":500}"
            "  ],"
            "  \"actuators\":[{\"id\":0,\"name\":\"f\","
            "                  \"pwm_min\":80,\"pwm_max\":255,"
            "                  \"state_pwm\":[0,100,160,220,255]}],"
            "  \"zones\":[{ \"name\":\"z\","
            "               \"sensors\":[\"a\",\"b\"],"
            "               \"sensor_weights_q16\":[32768],"
            "               \"aggregation\":\"weighted\","
            "               \"fallback_temp_mc\":85000,"
            "               \"governor\":\"step_wise\","
            "               \"actuators\":[\"f\"],"
            "               \"trips\":[{\"temp_mc\":70000,\"hyst_mc\":2000,"
            "                           \"severity\":\"warn\",\"cooling_state\":1}] }] }";
        thermal_status_t s = thermal_config_jsmn_parse(json, strlen(json),
                                                       &cfg, NULL, err, sizeof(err));
        EXPECT_EQ(s, THERMAL_ERR_INVALID_ARG);
        if (strstr(err, "sensor_weights_q16") == NULL) {
            fprintf(stderr,
                    "scenario 17: err did not name sensor_weights_q16: '%s'\n", err);
            exit(1);
        }
    }
#endif  /* Scenario 17 */

    /* === Scenarios 18-23: per-actuator fan_health block (Stage 17) ===
     * BASE has two %s slots: an optional ", \"tach\":..." key and an
     * optional ", \"fan_health\":{...}" block on the single actuator. */
    {
        static const char *BASE =
            "{ \"config_version\":1, \"control_period_ms\":100,"
            "  \"sensors\":[{\"id\":0,\"name\":\"soc\",\"iir_alpha_q16\":16384,"
            "                \"max_staleness_ms\":500}],"
            "  \"actuators\":[{\"id\":0,\"name\":\"f\",\"pwm_min\":80,"
            "                  \"pwm_max\":255,\"state_pwm\":[0,100,160,220,255]"
            "%s%s }],"
            "  \"zones\":[{ \"name\":\"z\",\"sensors\":[\"soc\"],"
            "               \"aggregation\":\"max\",\"fallback_temp_mc\":85000,"
            "               \"governor\":\"step_wise\",\"actuators\":[\"f\"],"
            "               \"trips\":[{\"temp_mc\":90000,\"hyst_mc\":2000,"
            "                           \"severity\":\"critical\","
            "                           \"cooling_state\":3}] }] }";
        const char *TACH = ", \"tach\":\"/sys/x/fan1_input\"";
        char buf[2048];

        /* 18: a valid fan_health block loads cleanly. */
        snprintf(buf, sizeof(buf), BASE, TACH,
            ", \"fan_health\":{ \"enable\":true, \"baseline_source\":\"field\","
            " \"baseline\":[[64,900],[128,1850],[255,2900]],"
            " \"stable_pwm_ticks\":300, \"stable_pwm_tolerance\":2,"
            " \"stable_rpm_ticks\":50, \"stable_rpm_tolerance_pct\":5,"
            " \"min_points_observed\":2,"
            " \"severity_pct\":{\"aging\":-5,\"degraded\":-15,\"failing\":-30} }");
        {
            thermal_status_t s = thermal_config_jsmn_parse(buf, strlen(buf),
                                                           &cfg, NULL,
                                                           err, sizeof(err));
            if (s != THERMAL_OK) {
                fprintf(stderr, "scenario 18: status=%d err='%s'\n", (int)s, err);
            }
            EXPECT_STATUS_OK(s);
            EXPECT_EQ(cfg.fan_health[0].enable, 1);
            EXPECT_EQ(cfg.fan_health[0].baseline_count, 3);
        }

        /* 19: a one-point baseline is rejected (needs >= 2 points). */
        snprintf(buf, sizeof(buf), BASE, TACH,
            ", \"fan_health\":{ \"enable\":true, \"baseline_source\":\"field\","
            " \"baseline\":[[64,900]],"
            " \"stable_pwm_ticks\":300, \"stable_pwm_tolerance\":2,"
            " \"stable_rpm_ticks\":50, \"stable_rpm_tolerance_pct\":5,"
            " \"min_points_observed\":1,"
            " \"severity_pct\":{\"aging\":-5,\"degraded\":-15,\"failing\":-30} }");
        EXPECT_EQ(thermal_config_jsmn_parse(buf, strlen(buf), &cfg, NULL,
                                            err, sizeof(err)),
                  THERMAL_ERR_INVALID_CONFIG);

        /* 20: baseline PWM values not strictly ascending (duplicate). */
        snprintf(buf, sizeof(buf), BASE, TACH,
            ", \"fan_health\":{ \"enable\":true, \"baseline_source\":\"field\","
            " \"baseline\":[[64,900],[64,1850],[255,2900]],"
            " \"stable_pwm_ticks\":300, \"stable_pwm_tolerance\":2,"
            " \"stable_rpm_ticks\":50, \"stable_rpm_tolerance_pct\":5,"
            " \"min_points_observed\":2,"
            " \"severity_pct\":{\"aging\":-5,\"degraded\":-15,\"failing\":-30} }");
        EXPECT_EQ(thermal_config_jsmn_parse(buf, strlen(buf), &cfg, NULL,
                                            err, sizeof(err)),
                  THERMAL_ERR_INVALID_CONFIG);

        /* 21: baseline RPM not monotonic non-decreasing. */
        snprintf(buf, sizeof(buf), BASE, TACH,
            ", \"fan_health\":{ \"enable\":true, \"baseline_source\":\"field\","
            " \"baseline\":[[64,900],[128,800],[255,2900]],"
            " \"stable_pwm_ticks\":300, \"stable_pwm_tolerance\":2,"
            " \"stable_rpm_ticks\":50, \"stable_rpm_tolerance_pct\":5,"
            " \"min_points_observed\":2,"
            " \"severity_pct\":{\"aging\":-5,\"degraded\":-15,\"failing\":-30} }");
        EXPECT_EQ(thermal_config_jsmn_parse(buf, strlen(buf), &cfg, NULL,
                                            err, sizeof(err)),
                  THERMAL_ERR_INVALID_CONFIG);

        /* 22: severity thresholds violate 0 > aging > degraded > failing. */
        snprintf(buf, sizeof(buf), BASE, TACH,
            ", \"fan_health\":{ \"enable\":true, \"baseline_source\":\"field\","
            " \"baseline\":[[64,900],[128,1850],[255,2900]],"
            " \"stable_pwm_ticks\":300, \"stable_pwm_tolerance\":2,"
            " \"stable_rpm_ticks\":50, \"stable_rpm_tolerance_pct\":5,"
            " \"min_points_observed\":2,"
            " \"severity_pct\":{\"aging\":-20,\"degraded\":-15,\"failing\":-30} }");
        EXPECT_EQ(thermal_config_jsmn_parse(buf, strlen(buf), &cfg, NULL,
                                            err, sizeof(err)),
                  THERMAL_ERR_INVALID_CONFIG);

        /* 23: an enabled fan_health block on a tachless actuator. */
        snprintf(buf, sizeof(buf), BASE, "",   /* no tach key */
            ", \"fan_health\":{ \"enable\":true, \"baseline_source\":\"field\","
            " \"baseline\":[[64,900],[128,1850],[255,2900]],"
            " \"stable_pwm_ticks\":300, \"stable_pwm_tolerance\":2,"
            " \"stable_rpm_ticks\":50, \"stable_rpm_tolerance_pct\":5,"
            " \"min_points_observed\":2,"
            " \"severity_pct\":{\"aging\":-5,\"degraded\":-15,\"failing\":-30} }");
        EXPECT_EQ(thermal_config_jsmn_parse(buf, strlen(buf), &cfg, NULL,
                                            err, sizeof(err)),
                  THERMAL_ERR_INVALID_ARG);
    }
}
