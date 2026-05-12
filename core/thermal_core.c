/* core/thermal_core.c
 *
 * Stage 4 commit 3a: thermal_core_validate_config is now a real
 * implementation covering 27 PRD §5.3 rules relevant to Stage 4's
 * aggregation + step-wise governor scope. The other four API functions
 * (init/step/apply_command/get_state) still return THERMAL_ERR_UNAVAILABLE
 * — the runtime wiring lands in commit 3b.
 *
 * The internal struct (thermal_core_internal_t) is still a placeholder.
 * Commit 3b extends it with per-sensor filter state, per-zone runtime
 * state, callbacks copy, and the const config pointer.
 */
#include "thermal_core.h"

typedef struct {
    /* Stage 4 commit 3b+: zone runtime state, sensor IIR filter state,
     * actuator slew state, PID integrator/derivative history, fault
     * detector state machines, context filter state, modifier state,
     * callbacks copy, const config pointer. */
    char _placeholder;          /* C99 requires at least one member */
} thermal_core_internal_t;

/* Compile-time fit check (C99 idiom). Negative array size triggers a
 * compile error if the internal struct outgrows the reserved buffer. */
typedef char thermal_core_t_fits[
    (sizeof(thermal_core_internal_t) <= sizeof(thermal_core_t)) ? 1 : -1];

/* === validate_config helpers === */

static int find_sensor_slot(const thermal_config_t *cfg, uint16_t id) {
    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        if (cfg->sensors[i].id == id) return (int)i;
    }
    return -1;
}

static int find_actuator_slot(const thermal_config_t *cfg, uint16_t id) {
    for (uint8_t i = 0; i < cfg->actuator_count; i++) {
        if (cfg->actuators[i].id == id) return (int)i;
    }
    return -1;
}

static int aggregation_known(uint8_t agg) {
    return agg == THERMAL_AGG_MAX
        || agg == THERMAL_AGG_AVG
        || agg == THERMAL_AGG_WEIGHTED;
}

static int governor_known(uint8_t gov) {
    return gov == THERMAL_GOVERNOR_STEP_WISE
        || gov == THERMAL_GOVERNOR_PID;
}

static int severity_known(uint8_t sev) {
    return sev == THERMAL_TRIP_WARN
        || sev == THERMAL_TRIP_CRITICAL
        || sev == THERMAL_TRIP_SHUTDOWN;
}

static thermal_status_t validate_sensors(const thermal_config_t *cfg) {
    /* Rule 11: max_staleness_ms > 0 per sensor */
    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        if (cfg->sensors[i].max_staleness_ms == 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
    }
    /* Rule 10: sensor IDs unique */
    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        for (uint8_t j = (uint8_t)(i + 1); j < cfg->sensor_count; j++) {
            if (cfg->sensors[i].id == cfg->sensors[j].id) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
    }
    return THERMAL_OK;
}

static thermal_status_t validate_actuators(const thermal_config_t *cfg) {
    /* Rules 13, 14 per actuator */
    for (uint8_t i = 0; i < cfg->actuator_count; i++) {
        const thermal_actuator_cfg_t *a = &cfg->actuators[i];
        if (a->pwm_min > a->pwm_max) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (a->spinup_ms > 0) {
            if (a->spinup_pwm < a->pwm_min || a->spinup_pwm > a->pwm_max) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
    }
    /* Rule 12: actuator IDs unique */
    for (uint8_t i = 0; i < cfg->actuator_count; i++) {
        for (uint8_t j = (uint8_t)(i + 1); j < cfg->actuator_count; j++) {
            if (cfg->actuators[i].id == cfg->actuators[j].id) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
    }
    return THERMAL_OK;
}

static thermal_status_t validate_contexts(const thermal_config_t *cfg) {
    /* Rule 15: context IDs unique */
    for (uint8_t i = 0; i < cfg->context_count; i++) {
        for (uint8_t j = (uint8_t)(i + 1); j < cfg->context_count; j++) {
            if (cfg->contexts[i].id == cfg->contexts[j].id) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
    }
    return THERMAL_OK;
}

static thermal_status_t validate_zone(const thermal_config_t *cfg,
                                      const thermal_zone_cfg_t *z) {
    /* Rule 16: sensor_count in [1, MAX_SENSORS_PER_ZONE] */
    if (z->sensor_count == 0 || z->sensor_count > THERMAL_MAX_SENSORS_PER_ZONE) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    /* Rule 17: actuator_count in [1, MAX_ACTUATORS_PER_ZONE] */
    if (z->actuator_count == 0 || z->actuator_count > THERMAL_MAX_ACTUATORS_PER_ZONE) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    /* Rule 20: aggregation in known set */
    if (!aggregation_known(z->aggregation)) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    /* Rule 21: governor in known set */
    if (!governor_known(z->governor)) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    /* Rule 18: each sensor_id resolves */
    for (uint8_t i = 0; i < z->sensor_count; i++) {
        if (find_sensor_slot(cfg, z->sensor_ids[i]) < 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
    }
    /* Rule 19: each actuator_id resolves */
    for (uint8_t i = 0; i < z->actuator_count; i++) {
        if (find_actuator_slot(cfg, z->actuator_ids[i]) < 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
    }
    /* Rule 22: weighted aggregation requires positive weight sum */
    if (z->aggregation == THERMAL_AGG_WEIGHTED) {
        int64_t wsum = 0;
        for (uint8_t i = 0; i < z->sensor_count; i++) {
            wsum += (int64_t)z->sensor_weights_q16[i];
        }
        if (wsum <= 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
    }
    /* Rule 23: trip_count <= MAX_TRIPS_PER_ZONE */
    if (z->trip_count > THERMAL_MAX_TRIPS_PER_ZONE) {
        return THERMAL_ERR_NO_SPACE;
    }
    /* Rules 24, 25, 26, 27: trip-array invariants */
    for (uint8_t i = 0; i < z->trip_count; i++) {
        const thermal_trip_cfg_t *t = &z->trips[i];
        /* Rule 26: cooling_state bounds (BOUNDS, not INVALID_CONFIG) */
        if (t->cooling_state >= THERMAL_MAX_COOLING_STATES) {
            return THERMAL_ERR_BOUNDS;
        }
        /* Rule 27: severity in known set */
        if (!severity_known(t->severity)) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (i > 0) {
            const thermal_trip_cfg_t *p = &z->trips[i - 1];
            /* Rule 24: strictly ascending temp_mc */
            if (t->temp_mc <= p->temp_mc) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
            /* Rule 25: hysteresis non-overlap.
             * trip[i].temp_mc - trip[i].hyst_mc >= trip[i-1].temp_mc.
             * Use int64 to avoid underflow if hyst_mc is large. */
            int64_t cold = (int64_t)t->temp_mc - (int64_t)t->hyst_mc;
            if (cold < (int64_t)p->temp_mc) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
    }
    return THERMAL_OK;
}

/* === Public API === */

thermal_status_t thermal_core_validate_config(const thermal_config_t *cfg) {
    /* Rule 1: NULL */
    if (!cfg) return THERMAL_ERR_INVALID_ARG;

    /* Rule 2: config_version */
    if (cfg->config_version != 1) return THERMAL_ERR_INVALID_CONFIG;

    /* Rule 3: control_period_ms > 0 */
    if (cfg->control_period_ms == 0) return THERMAL_ERR_INVALID_CONFIG;

    /* Rules 4-8: array counts within compile-time maxima */
    if (cfg->sensor_count   > THERMAL_MAX_SENSORS)         return THERMAL_ERR_NO_SPACE;
    if (cfg->zone_count     > THERMAL_MAX_ZONES)           return THERMAL_ERR_NO_SPACE;
    if (cfg->actuator_count > THERMAL_MAX_ACTUATORS)       return THERMAL_ERR_NO_SPACE;
    if (cfg->context_count  > THERMAL_MAX_CONTEXT_SIGNALS) return THERMAL_ERR_NO_SPACE;
    if (cfg->modifier_count > THERMAL_MAX_MODIFIERS)       return THERMAL_ERR_NO_SPACE;
    if (cfg->telemetry.enabled_signal_count > THERMAL_MAX_TELEMETRY_SIGNALS) {
        return THERMAL_ERR_NO_SPACE;
    }

    /* Rule 9: v1 modifier limit (0 or 1) */
    if (cfg->modifier_count > 1) return THERMAL_ERR_INVALID_CONFIG;

    /* Per-array checks */
    thermal_status_t s;
    if ((s = validate_sensors(cfg))   != THERMAL_OK) return s;
    if ((s = validate_actuators(cfg)) != THERMAL_OK) return s;
    if ((s = validate_contexts(cfg))  != THERMAL_OK) return s;

    /* Per-zone checks */
    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        if ((s = validate_zone(cfg, &cfg->zones[i])) != THERMAL_OK) return s;
    }

    return THERMAL_OK;
}

thermal_status_t thermal_core_init(thermal_core_t *ctx,
                                   const thermal_config_t *cfg,
                                   const thermal_core_callbacks_t *cb) {
    (void)ctx; (void)cfg; (void)cb;
    return THERMAL_ERR_UNAVAILABLE;
}

thermal_status_t thermal_core_step(thermal_core_t *ctx,
                                   const thermal_input_snapshot_t *in,
                                   thermal_output_frame_t *out) {
    (void)ctx; (void)in; (void)out;
    return THERMAL_ERR_UNAVAILABLE;
}

thermal_status_t thermal_core_apply_command(thermal_core_t *ctx,
                                            uint32_t now_ms,
                                            const thermal_command_t *cmd,
                                            thermal_command_result_t *result) {
    (void)ctx; (void)now_ms; (void)cmd; (void)result;
    return THERMAL_ERR_UNAVAILABLE;
}

thermal_status_t thermal_core_get_state(const thermal_core_t *ctx,
                                        thermal_state_snapshot_t *state) {
    (void)ctx; (void)state;
    return THERMAL_ERR_UNAVAILABLE;
}
