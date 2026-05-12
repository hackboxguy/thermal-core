/* core/thermal_core.c
 *
 * Stage 4 commit 3b closes Stage 4. thermal_core_step now runs §4.6
 * control-loop steps 3 (filters) and 6 (aggregation + step-wise
 * governor for step-wise zones). thermal_core_init validates the
 * config, copies callbacks, and builds the per-zone resolved-sensor
 * lookup tables. thermal_core_get_state populates the zone fields
 * of thermal_state_snapshot_t from internal state.
 *
 * Output frame at this stage is a zero-filled placeholder per the
 * agreed Stage-4 contract gap; Stage 7 (arbitration + slew + modifier)
 * fills it with real actuator commands.
 *
 * apply_command still returns THERMAL_ERR_UNAVAILABLE — Stage 8.
 */
#include <string.h>            /* memset */
#include "thermal_core.h"
#include "thermal_filter.h"
#include "thermal_zone.h"
#include "thermal_governor.h"
#include "thermal_pid.h"

/* === Internal state === */

typedef struct {
    int32_t  temp_mc;            /* last aggregation result */
    uint32_t active_trip_mask;   /* hysteresis state across ticks */
    uint8_t  cooling_state;      /* last governor output */
    uint8_t  aggregation_valid;  /* 1 if last aggregation had >= 1 valid sensor */
    uint8_t  sensor_slot_count;  /* copy of cfg.zones[i].sensor_count */
    uint8_t  sensor_slots[THERMAL_MAX_SENSORS_PER_ZONE];  /* resolved at init */
    thermal_pid_state_t pid;     /* PID integrator + derivative history; carried
                                  * for every zone (Stage 5b). step-wise zones
                                  * leave it untouched. */
} zone_runtime_t;

typedef struct {
    const thermal_config_t   *cfg;
    thermal_core_callbacks_t  cb;
    thermal_filter_state_t    filters[THERMAL_MAX_SENSORS];
    zone_runtime_t            zones[THERMAL_MAX_ZONES];
    uint32_t                  last_now_ms;
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
    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        if (cfg->sensors[i].max_staleness_ms == 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
    }
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
    if (z->sensor_count == 0 || z->sensor_count > THERMAL_MAX_SENSORS_PER_ZONE) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    if (z->actuator_count == 0 || z->actuator_count > THERMAL_MAX_ACTUATORS_PER_ZONE) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    if (!aggregation_known(z->aggregation)) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    if (!governor_known(z->governor)) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    for (uint8_t i = 0; i < z->sensor_count; i++) {
        if (find_sensor_slot(cfg, z->sensor_ids[i]) < 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
    }
    for (uint8_t i = 0; i < z->actuator_count; i++) {
        if (find_actuator_slot(cfg, z->actuator_ids[i]) < 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
    }
    if (z->aggregation == THERMAL_AGG_WEIGHTED) {
        int64_t wsum = 0;
        for (uint8_t i = 0; i < z->sensor_count; i++) {
            wsum += (int64_t)z->sensor_weights_q16[i];
        }
        if (wsum <= 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
    }
    if (z->trip_count > THERMAL_MAX_TRIPS_PER_ZONE) {
        return THERMAL_ERR_NO_SPACE;
    }
    for (uint8_t i = 0; i < z->trip_count; i++) {
        const thermal_trip_cfg_t *t = &z->trips[i];
        if (t->cooling_state >= THERMAL_MAX_COOLING_STATES) {
            return THERMAL_ERR_BOUNDS;
        }
        if (!severity_known(t->severity)) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (i > 0) {
            const thermal_trip_cfg_t *p = &z->trips[i - 1];
            if (t->temp_mc <= p->temp_mc) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
            int64_t cold = (int64_t)t->temp_mc - (int64_t)t->hyst_mc;
            if (cold < (int64_t)p->temp_mc) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
    }

    /* PID-zone-specific rules (PRD §5.3 lines 796-798) */
    if (z->governor == THERMAL_GOVERNOR_PID) {
        const thermal_pid_cfg_t *p = &z->pid;
        /* Rule 28: gain bounds */
        if (p->kp_q16 < p->kp_min_q16 || p->kp_q16 > p->kp_max_q16) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (p->ki_q16 < p->ki_min_q16 || p->ki_q16 > p->ki_max_q16) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (p->kd_q16 < p->kd_min_q16 || p->kd_q16 > p->kd_max_q16) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 29: setpoint bounds */
        if (p->setpoint_mc < p->setpoint_min_mc ||
            p->setpoint_mc > p->setpoint_max_mc) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 30: dt clamp sanity */
        if (p->dt_min_ms == 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (p->dt_min_ms > cfg->control_period_ms) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (p->dt_max_ms < cfg->control_period_ms) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 31: at least one critical or shutdown trip (safety floor) */
        int has_floor = 0;
        for (uint8_t i = 0; i < z->trip_count; i++) {
            if (z->trips[i].severity == THERMAL_TRIP_CRITICAL ||
                z->trips[i].severity == THERMAL_TRIP_SHUTDOWN) {
                has_floor = 1;
                break;
            }
        }
        if (!has_floor) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
    }

    return THERMAL_OK;
}

/* === Public API === */

thermal_status_t thermal_core_validate_config(const thermal_config_t *cfg) {
    if (!cfg) return THERMAL_ERR_INVALID_ARG;
    if (cfg->config_version != 1) return THERMAL_ERR_INVALID_CONFIG;
    if (cfg->control_period_ms == 0) return THERMAL_ERR_INVALID_CONFIG;

    if (cfg->sensor_count   > THERMAL_MAX_SENSORS)         return THERMAL_ERR_NO_SPACE;
    if (cfg->zone_count     > THERMAL_MAX_ZONES)           return THERMAL_ERR_NO_SPACE;
    if (cfg->actuator_count > THERMAL_MAX_ACTUATORS)       return THERMAL_ERR_NO_SPACE;
    if (cfg->context_count  > THERMAL_MAX_CONTEXT_SIGNALS) return THERMAL_ERR_NO_SPACE;
    if (cfg->modifier_count > THERMAL_MAX_MODIFIERS)       return THERMAL_ERR_NO_SPACE;
    if (cfg->telemetry.enabled_signal_count > THERMAL_MAX_TELEMETRY_SIGNALS) {
        return THERMAL_ERR_NO_SPACE;
    }

    if (cfg->modifier_count > 1) return THERMAL_ERR_INVALID_CONFIG;

    /* Rule 32: runaway detector persist_ticks must fit the per-instance
     * ring buffer (Stage 6 commit 6a). */
    if (cfg->faults.runaway_defaults.enabled &&
        cfg->faults.runaway_defaults.persist_ticks > THERMAL_FAULT_RUNAWAY_WINDOW_MAX) {
        return THERMAL_ERR_BOUNDS;
    }

    thermal_status_t s;
    if ((s = validate_sensors(cfg))   != THERMAL_OK) return s;
    if ((s = validate_actuators(cfg)) != THERMAL_OK) return s;
    if ((s = validate_contexts(cfg))  != THERMAL_OK) return s;

    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        if ((s = validate_zone(cfg, &cfg->zones[i])) != THERMAL_OK) return s;
    }

    return THERMAL_OK;
}

thermal_status_t thermal_core_init(thermal_core_t *ctx,
                                   const thermal_config_t *cfg,
                                   const thermal_core_callbacks_t *cb) {
    if (!ctx || !cfg || !cb) return THERMAL_ERR_INVALID_ARG;

    thermal_status_t s = thermal_core_validate_config(cfg);
    if (s != THERMAL_OK) return s;

    thermal_core_internal_t *core = (thermal_core_internal_t *)ctx;
    core->cfg = cfg;
    core->cb = *cb;
    core->last_now_ms = 0;

    for (uint8_t i = 0; i < THERMAL_MAX_SENSORS; i++) {
        thermal_filter_reset(&core->filters[i]);
    }

    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        zone_runtime_t *zr = &core->zones[i];
        const thermal_zone_cfg_t *zc = &cfg->zones[i];
        zr->temp_mc = 0;
        zr->active_trip_mask = 0;
        zr->cooling_state = 0;
        zr->aggregation_valid = 0;
        zr->sensor_slot_count = zc->sensor_count;
        for (uint8_t k = 0; k < zc->sensor_count; k++) {
            /* validate_config guarantees this resolves */
            zr->sensor_slots[k] = (uint8_t)find_sensor_slot(cfg, zc->sensor_ids[k]);
        }
        /* Pad unused slots with 0 for determinism (not strictly needed). */
        for (uint8_t k = zc->sensor_count; k < THERMAL_MAX_SENSORS_PER_ZONE; k++) {
            zr->sensor_slots[k] = 0;
        }
        thermal_pid_reset(&zr->pid);
    }

    /* Pad unused zone slots so state-snapshot reads are well-defined. */
    for (uint8_t i = cfg->zone_count; i < THERMAL_MAX_ZONES; i++) {
        zone_runtime_t *zr = &core->zones[i];
        zr->temp_mc = 0;
        zr->active_trip_mask = 0;
        zr->cooling_state = 0;
        zr->aggregation_valid = 0;
        zr->sensor_slot_count = 0;
        for (uint8_t k = 0; k < THERMAL_MAX_SENSORS_PER_ZONE; k++) {
            zr->sensor_slots[k] = 0;
        }
        thermal_pid_reset(&zr->pid);
    }

    return THERMAL_OK;
}

thermal_status_t thermal_core_step(thermal_core_t *ctx,
                                   const thermal_input_snapshot_t *in,
                                   thermal_output_frame_t *out) {
    if (!ctx || !in || !out) return THERMAL_ERR_INVALID_ARG;
    if (in->sample_count > THERMAL_MAX_SAMPLES_PER_SNAPSHOT) {
        return THERMAL_ERR_NO_SPACE;
    }

    thermal_core_internal_t *core = (thermal_core_internal_t *)ctx;
    const thermal_config_t *cfg = core->cfg;

    /* === §4.6 step 3: dispatch samples to filters === */
    for (uint8_t s = 0; s < in->sample_count; s++) {
        const thermal_sample_t *smp = &in->samples[s];
        /* Stage 4: only TEMP_MC samples processed. Tach (RPM) and context
         * (I32) samples are silently skipped until Stage 7+. */
        if (smp->kind != THERMAL_SAMPLE_TEMP_MC) continue;
        int slot = find_sensor_slot(cfg, smp->id);
        if (slot < 0) continue; /* unknown sensor: skip silently */
        const thermal_sensor_cfg_t *sc = &cfg->sensors[slot];
        thermal_filter_step(&core->filters[slot],
                            sc->iir_alpha_q16, smp->value, smp->valid);
    }

    /* === §4.6 step 6: aggregate per zone, run step-wise governor === */
    int32_t  filter_values[THERMAL_MAX_SENSORS_PER_ZONE];
    uint8_t  filter_valid [THERMAL_MAX_SENSORS_PER_ZONE];
    int32_t  weights      [THERMAL_MAX_SENSORS_PER_ZONE];

    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        zone_runtime_t *zr = &core->zones[i];
        const thermal_zone_cfg_t *zc = &cfg->zones[i];

        for (uint8_t k = 0; k < zc->sensor_count; k++) {
            uint8_t slot = zr->sensor_slots[k];
            filter_values[k] = core->filters[slot].filtered_value;
            filter_valid[k]  = core->filters[slot].valid;
            weights[k]       = zc->sensor_weights_q16[k];
        }

        thermal_zone_aggregate_result_t agg;
        thermal_zone_aggregate((thermal_aggregation_t)zc->aggregation,
                               filter_values, filter_valid, weights,
                               zc->sensor_count, zc->fallback_temp_mc, &agg);

        zr->temp_mc = agg.temp_mc;
        zr->aggregation_valid = agg.valid;

        if (zc->governor == THERMAL_GOVERNOR_STEP_WISE) {
            thermal_governor_step_result_t gr;
            thermal_governor_step_wise(agg.temp_mc, zc->trips, zc->trip_count,
                                       zr->active_trip_mask, &gr);
            zr->active_trip_mask = gr.active_trip_mask;
            zr->cooling_state    = gr.cooling_state;
        } else {
            /* PID zone (Stage 5b). Run PID for the integrator/derivative
             * history; output PWM is discarded until Stage 7 arbitration
             * fills the output frame. pwm_min/pwm_max are the conservative
             * full range; per-actuator clamping happens at arbitration. */
            thermal_pid_step_result_t pr;
            thermal_pid_step(&zr->pid,
                             zc->pid.kp_q16, zc->pid.ki_q16, zc->pid.kd_q16,
                             zc->pid.setpoint_mc, agg.temp_mc,
                             in->now_ms,
                             zc->pid.dt_min_ms, zc->pid.dt_max_ms,
                             0, 255,
                             &pr);
            (void)pr;

            /* active_trip_mask reflects all active trips (WARN + CRITICAL +
             * SHUTDOWN) for telemetry visibility; cooling_state filters to
             * critical/shutdown only per PRD §4.8 line 670. */
            thermal_governor_step_result_t gr;
            thermal_governor_step_wise(agg.temp_mc, zc->trips, zc->trip_count,
                                       zr->active_trip_mask, &gr);
            zr->active_trip_mask = gr.active_trip_mask;

            uint8_t pid_cs = 0;
            for (uint8_t k = 0; k < zc->trip_count; k++) {
                if (!(gr.active_trip_mask & ((uint32_t)1u << k))) continue;
                const thermal_trip_cfg_t *t = &zc->trips[k];
                if (t->severity != THERMAL_TRIP_CRITICAL &&
                    t->severity != THERMAL_TRIP_SHUTDOWN) continue;
                if (t->cooling_state > pid_cs) pid_cs = t->cooling_state;
            }
            zr->cooling_state = pid_cs;
        }
    }

    /* === §4.6 step 10 (placeholder): zero-fill output frame === */
    out->actuator_cmd_count = cfg->actuator_count;
    for (uint8_t i = 0; i < cfg->actuator_count; i++) {
        out->actuator_cmds[i].actuator_id = cfg->actuators[i].id;
        out->actuator_cmds[i].duty_0_255 = 0;
        out->actuator_cmds[i].reason = THERMAL_ACT_REASON_NONE;
    }

    core->last_now_ms = in->now_ms;
    return THERMAL_OK;
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
    if (!ctx || !state) return THERMAL_ERR_INVALID_ARG;
    const thermal_core_internal_t *core = (const thermal_core_internal_t *)ctx;
    const thermal_config_t *cfg = core->cfg;

    memset(state, 0, sizeof(*state));
    state->now_ms = core->last_now_ms;
    state->zone_count     = cfg->zone_count;
    state->actuator_count = cfg->actuator_count;
    state->fault_count    = 0;          /* Stage 6 */
    state->context_count  = 0;          /* Stage 7 */
    state->modifier_count = 0;          /* Stage 7 */
    state->flags          = 0;

    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        const zone_runtime_t *zr = &core->zones[i];
        const thermal_zone_cfg_t *zc = &cfg->zones[i];
        state->zones[i].temp_mc          = zr->temp_mc;
        state->zones[i].active_trip_mask = zr->active_trip_mask;
        state->zones[i].cooling_state    = zr->cooling_state;
        state->zones[i].effective_setpoint_mc =
            (zc->governor == THERMAL_GOVERNOR_PID) ? zc->pid.setpoint_mc : 0;
        /* Stage 7's pre-governor acoustic_mask modifier will eventually
         * offset this; "effective" == "configured" for now. */
    }
    /* actuators[], contexts[], faults[], modifiers[] stay zero. */
    return THERMAL_OK;
}
