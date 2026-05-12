/* test/property/property_config.c
 *
 * Property-test harness for thermal_core_validate_config. Generates N
 * random thermal_config_t instances deterministically from sequential
 * LCG seeds and emits a CSV of (seed, returned_status) to stdout.
 *
 * test/property/run_property.py invokes this binary and asserts every
 * returned status is one of the documented thermal_status_t values
 * (THERMAL_OK..THERMAL_ERR_REJECTED_SAFETY). Anything else is a
 * property violation (undocumented return value, crash, etc.).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "thermal_core.h"

static uint32_t lcg_state;
static uint32_t lcg(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state;
}

static void fill_random_name(char *buf, int len) {
    for (int j = 0; j < len - 1; j++) {
        buf[j] = (char)('a' + (lcg() & 7));
    }
    buf[len - 1] = 0;
}

static void generate_random_config(uint32_t seed, thermal_config_t *cfg) {
    lcg_state = seed;
    memset(cfg, 0, sizeof(*cfg));

    cfg->config_version    = (uint16_t)(lcg() & 3);     /* 0..3, only 1 is valid */
    cfg->control_period_ms = (uint16_t)(lcg() & 0x3FF); /* 0..1023; often 0 */

    cfg->sensor_count   = (uint8_t)(lcg() % (THERMAL_MAX_SENSORS + 2));
    cfg->zone_count     = (uint8_t)(lcg() % (THERMAL_MAX_ZONES + 2));
    cfg->actuator_count = (uint8_t)(lcg() % (THERMAL_MAX_ACTUATORS + 2));
    cfg->context_count  = (uint8_t)(lcg() % (THERMAL_MAX_CONTEXT_SIGNALS + 2));
    cfg->modifier_count = (uint8_t)(lcg() % (THERMAL_MAX_MODIFIERS + 2));
    cfg->telemetry.enabled_signal_count =
        (uint8_t)(lcg() % (THERMAL_MAX_TELEMETRY_SIGNALS + 2));

    /* Fill array slots up to MIN(count, MAX). When count > MAX, validate
     * returns NO_SPACE before reading the array, so the contents beyond
     * MAX don't matter. */
    uint8_t n;

    n = cfg->sensor_count < THERMAL_MAX_SENSORS ? cfg->sensor_count : THERMAL_MAX_SENSORS;
    for (uint8_t i = 0; i < n; i++) {
        cfg->sensors[i].id = (uint16_t)(lcg() & 0xFF);
        cfg->sensors[i].iir_alpha_q16 = (int32_t)lcg();
        cfg->sensors[i].max_staleness_ms = lcg() & 0xFFFF;
        fill_random_name(cfg->sensors[i].name, THERMAL_NAME_MAX);
    }

    n = cfg->actuator_count < THERMAL_MAX_ACTUATORS ? cfg->actuator_count : THERMAL_MAX_ACTUATORS;
    for (uint8_t i = 0; i < n; i++) {
        cfg->actuators[i].id = (uint16_t)(lcg() & 0xFF);
        cfg->actuators[i].pwm_min = (uint8_t)(lcg() & 0xFF);
        cfg->actuators[i].pwm_max = (uint8_t)(lcg() & 0xFF);
        cfg->actuators[i].slew_per_tick = (uint8_t)(lcg() & 0xFF);
        cfg->actuators[i].spinup_pwm = (uint8_t)(lcg() & 0xFF);
        cfg->actuators[i].spinup_ms = lcg() & 0xFFFF;
        for (uint8_t s = 0; s < THERMAL_MAX_COOLING_STATES; s++) {
            cfg->actuators[i].state_pwm[s] = (uint8_t)(lcg() & 0xFF);
        }
        fill_random_name(cfg->actuators[i].name, THERMAL_NAME_MAX);
    }

    n = cfg->context_count < THERMAL_MAX_CONTEXT_SIGNALS
        ? cfg->context_count : THERMAL_MAX_CONTEXT_SIGNALS;
    for (uint8_t i = 0; i < n; i++) {
        cfg->contexts[i].id = (uint16_t)(lcg() & 0xFF);
        cfg->contexts[i].unit = (uint8_t)(lcg() & 7);
        cfg->contexts[i].iir_alpha_q16 = (int32_t)lcg();
        cfg->contexts[i].timeout_ms = lcg() & 0xFFFF;
        cfg->contexts[i].fail_safe = (uint8_t)(lcg() & 7);
        fill_random_name(cfg->contexts[i].name, THERMAL_NAME_MAX);
    }

    n = cfg->zone_count < THERMAL_MAX_ZONES ? cfg->zone_count : THERMAL_MAX_ZONES;
    for (uint8_t i = 0; i < n; i++) {
        thermal_zone_cfg_t *z = &cfg->zones[i];
        fill_random_name(z->name, THERMAL_NAME_MAX);
        z->sensor_count = (uint8_t)(lcg() % (THERMAL_MAX_SENSORS_PER_ZONE + 2));
        for (uint8_t k = 0; k < THERMAL_MAX_SENSORS_PER_ZONE; k++) {
            z->sensor_ids[k] = (uint16_t)(lcg() & 0xFF);
            z->sensor_weights_q16[k] = (int32_t)lcg();
        }
        z->aggregation = (uint8_t)(lcg() & 7);
        z->fallback_temp_mc = (int32_t)lcg();
        z->governor = (uint8_t)(lcg() & 7);
        z->actuator_count = (uint8_t)(lcg() % (THERMAL_MAX_ACTUATORS_PER_ZONE + 2));
        for (uint8_t k = 0; k < THERMAL_MAX_ACTUATORS_PER_ZONE; k++) {
            z->actuator_ids[k] = (uint16_t)(lcg() & 0xFF);
        }
        z->trip_count = (uint8_t)(lcg() % (THERMAL_MAX_TRIPS_PER_ZONE + 2));
        for (uint8_t k = 0; k < THERMAL_MAX_TRIPS_PER_ZONE; k++) {
            z->trips[k].temp_mc = (int32_t)lcg();
            z->trips[k].hyst_mc = (int32_t)(lcg() & 0xFFFF);
            z->trips[k].severity = (uint8_t)(lcg() & 7);
            z->trips[k].cooling_state = (uint8_t)(lcg() & 7);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <n_seeds>\n", argv[0]);
        return 2;
    }
    int n_signed = atoi(argv[1]);
    if (n_signed <= 0) {
        fprintf(stderr, "n_seeds must be positive\n");
        return 2;
    }
    uint32_t n = (uint32_t)n_signed;

    thermal_config_t cfg;
    printf("seed,status\n");
    for (uint32_t seed = 1; seed <= n; seed++) {
        generate_random_config(seed, &cfg);
        thermal_status_t s = thermal_core_validate_config(&cfg);
        printf("%u,%d\n", seed, (int)s);
    }
    return 0;
}
