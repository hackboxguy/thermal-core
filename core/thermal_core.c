/* core/thermal_core.c
 *
 * Stage 7 commit 7c closes the output-frame contract gap that has been
 * open since Stage 4. thermal_core_step() now runs every step of the
 * PRD §4.6 control loop (3 through 11), populating the actuator output
 * frame and every array of thermal_state_snapshot_t with real values.
 *
 * Module wiring (each module unit-tested in earlier stages):
 *   Step 3  thermal_filter           (Stage 3)
 *   Step 4  thermal_filter           (per-context, Stage 7c)
 *   Step 5  thermal_modifier_eval    (Stage 7a)
 *   Step 6  thermal_zone_aggregate + thermal_governor_step_wise +
 *           thermal_pid_step         (Stage 5b, with trip-offset shift)
 *   Step 7  thermal_arbitrator_max_wins   (Stage 7b)
 *   Step 8  modifier pwm_cap         (this commit)
 *   Step 9  thermal_fault_*_step     (Stage 6)
 *   Step 10 thermal_slew_step + final clamp   (Stage 7b)
 *   Step 11 telemetry_emit on cadence (this commit)
 *
 * Stage 8 complete; apply_command implemented for the 5 v1 commands
 * per PRD §7.5. Stage 9 begins next.
 */
#include <string.h>            /* memset */
#include "thermal_core.h"
#include "thermal_filter.h"
#include "thermal_zone.h"
#include "thermal_governor.h"
#include "thermal_pid.h"
#include "thermal_modifier.h"
#include "thermal_arbitrator.h"
#include "thermal_slew.h"
#include "thermal_fault.h"
#include "thermal_events.h"
#include "thermal_signals.h"

/* === Internal state === */

typedef struct {
    int32_t  temp_mc;            /* last aggregation result */
    uint32_t active_trip_mask;   /* hysteresis state across ticks */
    uint8_t  cooling_state;      /* last governor output */
    uint8_t  aggregation_valid;  /* 1 if last aggregation had >= 1 valid sensor */
    uint8_t  aggregation_seen;   /* edge memory for fallback enter/exit events */
    uint8_t  sensor_slot_count;  /* copy of cfg.zones[i].sensor_count */
    uint8_t  sensor_slots[THERMAL_MAX_SENSORS_PER_ZONE];  /* resolved at init */
    thermal_pid_state_t pid;     /* PID integrator + derivative history (Stage 5b) */
    /* Stage 8: runtime-mutable shadow of cfg fields commands can change.
     * Copied from cfg at init; mutated by thermal_core_apply_command.
     * The step function reads from these (not cfg->zones[i].pid / .trips). */
    thermal_pid_cfg_t   shadow_pid;
    thermal_trip_cfg_t  shadow_trips[THERMAL_MAX_TRIPS_PER_ZONE];
    /* Stage 7c: handoff into step-7 arbitration */
    uint8_t  pwm_this_tick;      /* PID zone's PWM demand for this tick;
                                  * step-wise zones derive per-actuator from
                                  * state_pwm[cooling_state] inside step 7. */
    /* Stage 7c: captured PID telemetry components */
    int32_t  pid_p_term_q16;
    int32_t  pid_i_term_q16;
    int32_t  pid_d_term_q16;
    int32_t  pid_error_mc;       /* temp_mc - effective_setpoint_mc */
    int32_t  effective_setpoint_mc;
} zone_runtime_t;

typedef struct {
    const thermal_config_t   *cfg;
    thermal_core_callbacks_t  cb;

    thermal_filter_state_t    filters[THERMAL_MAX_SENSORS];
    zone_runtime_t            zones[THERMAL_MAX_ZONES];

    /* Stage 7c: context-signal filtering + staleness (step 4) */
    thermal_filter_state_t    context_filters[THERMAL_MAX_CONTEXT_SIGNALS];
    uint32_t                  context_last_valid_ms[THERMAL_MAX_CONTEXT_SIGNALS];
    uint8_t                   context_ever_valid[THERMAL_MAX_CONTEXT_SIGNALS];
    uint8_t                   context_fail_safe_active[THERMAL_MAX_CONTEXT_SIGNALS];
    int32_t                   context_effective_value[THERMAL_MAX_CONTEXT_SIGNALS];

    /* Stage 7c: per-actuator carry into next tick (also fed to state-snapshot) */
    uint8_t                   actuator_prev_duty[THERMAL_MAX_ACTUATORS];
    uint8_t                   actuator_prev_slew_limited[THERMAL_MAX_ACTUATORS];
    uint8_t                   actuator_prev_requested[THERMAL_MAX_ACTUATORS];
    uint16_t                  actuator_prev_reason[THERMAL_MAX_ACTUATORS];
    uint8_t                   actuator_prev_safety_override[THERMAL_MAX_ACTUATORS];
    uint16_t                  actuator_spinup_remaining[THERMAL_MAX_ACTUATORS];
    uint16_t                  actuator_on_ticks[THERMAL_MAX_ACTUATORS];
    uint16_t                  actuator_off_ticks[THERMAL_MAX_ACTUATORS];

    /* Stage 8.5 codex-B: per-actuator tach state populated by step 3b
     * from THERMAL_SAMPLE_TACH_RPM samples (one per tick per actuator).
     * Fed into the stall detector + reported via state snapshot and
     * telemetry signal TSIG_ACTUATOR_RPM_MAIN_FAN. */
    uint16_t                  actuator_tach_rpm[THERMAL_MAX_ACTUATORS];
    uint8_t                   actuator_tach_valid[THERMAL_MAX_ACTUATORS];

    /* Stage 8.5 codex-B: one-way latch set on any SHUTDOWN-trip rising
     * edge OR any active detector with REQUEST_SHUTDOWN action. Stays
     * set across ticks until thermal_core_init clears it (PRD §4.7
     * "latch a shutdown-request condition"). Reported via
     * THERMAL_STATE_SHUTDOWN_REQUESTED. */
    uint8_t                   shutdown_request_latched;

    /* Stage 7c: per-modifier last-tick eval (steps 5/8) */
    uint8_t                   modifier_active[THERMAL_MAX_MODIFIERS];
    int32_t                   modifier_pwm_cap[THERMAL_MAX_MODIFIERS];
    int32_t                   modifier_trip_offset[THERMAL_MAX_MODIFIERS];

    /* Stage 8: per-modifier shadow of cfg fields commands can change.
     * Copied from cfg at init; CMD_SET_CURVE_POINT mutates the curve.
     * step5_modifier_eval reads from this (not cfg->modifiers). */
    thermal_modifier_cfg_t    shadow_modifiers[THERMAL_MAX_MODIFIERS];

    /* Stage 7c: fault-detector instances (PRD §4.7 — one per target) */
    thermal_fault_stall_state_t          stall_states[THERMAL_MAX_ACTUATORS];
    thermal_fault_stuck_sensor_state_t   stuck_sensor_states[THERMAL_MAX_SENSORS];
    thermal_fault_runaway_state_t        runaway_states[THERMAL_MAX_ZONES];
    thermal_fault_stale_context_state_t  stale_context_states[THERMAL_MAX_CONTEXT_SIGNALS];
    uint16_t                             runaway_latched_active_ticks[THERMAL_MAX_ZONES];
    uint8_t                              runaway_escalated_shutdown[THERMAL_MAX_ZONES];
    int32_t                              stuck_context_prev_value[THERMAL_MAX_CONTEXT_SIGNALS];
    uint8_t                              stuck_context_prev_valid[THERMAL_MAX_CONTEXT_SIGNALS];

#if THERMALCORE_ENABLE_FAN_HEALTH
    /* Stage 17 (PRD Appendix C): per-actuator fan-health detector. */
    thermal_fan_health_state_t           fan_health_states[THERMAL_MAX_ACTUATORS];
#endif

    /* Stage 7c: edge memory for event emission */
    uint8_t                   prev_zone_max_severity[THERMAL_MAX_ZONES];

    /* Stage 7c: telemetry cadence */
    uint32_t                  tick_count;

    uint32_t                  last_now_ms;
} thermal_core_internal_t;

/* Compile-time fit check (C99 idiom). */
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

static int find_context_slot(const thermal_config_t *cfg, uint16_t id) {
    for (uint8_t i = 0; i < cfg->context_count; i++) {
        if (cfg->contexts[i].id == id) return (int)i;
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

/* Forward decl: defined alongside dispatch_signal_value below. Used by
 * validate_telemetry to check enabled_signal_ids[] decode to supported
 * (range, sub, slot) tuples for this cfg. */
static int signal_id_is_supported(const thermal_config_t *cfg, uint16_t sig);

static thermal_status_t validate_sensors(const thermal_config_t *cfg) {
    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        if (cfg->sensors[i].max_staleness_ms == 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 41 (codex-C v1-#8): IIR coefficient must be in
         * [0, Q16_ONE] per PRD §4.5 ("0..Q16_ONE in normal use"). */
        if (cfg->sensors[i].iir_alpha_q16 < 0 ||
            cfg->sensors[i].iir_alpha_q16 > Q16_ONE) {
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
            if (a->spinup_pwm == 0 ||
                a->spinup_pwm < a->pwm_min ||
                a->spinup_pwm > a->pwm_max) {
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

static int adjusted_trips_valid(const thermal_trip_cfg_t *trips,
                                uint8_t trip_count,
                                int32_t offset_mc) {
    thermal_trip_cfg_t prev;
    for (uint8_t i = 0; i < trip_count; i++) {
        int64_t shifted = (int64_t)trips[i].temp_mc + (int64_t)offset_mc;
        if (shifted < (int64_t)INT32_MIN || shifted > (int64_t)INT32_MAX) {
            return 0;
        }
        thermal_trip_cfg_t curr = trips[i];
        curr.temp_mc = (int32_t)shifted;
        if (i > 0) {
            if (curr.temp_mc <= prev.temp_mc) {
                return 0;
            }
            if ((int64_t)curr.temp_mc - (int64_t)curr.hyst_mc <
                (int64_t)prev.temp_mc) {
                return 0;
            }
        }
        prev = curr;
    }
    return 1;
}

static int zone_fallback_reaches_safety_floor(const thermal_zone_cfg_t *z,
                                              const thermal_trip_cfg_t *trips) {
    int have_critical = 0;
    int have_any_trip = 0;
    int32_t highest_critical_mc = 0;
    int32_t highest_trip_mc = 0;
    for (uint8_t i = 0; i < z->trip_count; i++) {
        if (!have_any_trip || trips[i].temp_mc > highest_trip_mc) {
            highest_trip_mc = trips[i].temp_mc;
            have_any_trip = 1;
        }
        if (trips[i].severity == THERMAL_TRIP_CRITICAL) {
            if (!have_critical || trips[i].temp_mc > highest_critical_mc) {
                highest_critical_mc = trips[i].temp_mc;
            }
            have_critical = 1;
        }
    }
    if (have_critical) {
        return z->fallback_temp_mc >= highest_critical_mc;
    }
    return !have_any_trip || z->fallback_temp_mc >= highest_trip_mc;
}

static int modifier_curve_values_valid(const thermal_config_t *cfg,
                                       const thermal_modifier_cfg_t *m) {
    for (uint8_t p = 0; p < m->curve_count; p++) {
        int32_t cap = m->curve[p].value0;
        int32_t offset = m->curve[p].value1;
        if (cap < 0 || cap > 255) {
            return 0;
        }
        if (m->stages & THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP) {
            for (uint8_t a = 0; a < cfg->actuator_count; a++) {
                if (cap != 0 && cap < cfg->actuators[a].pwm_min) {
                    return 0;
                }
            }
        }
        if (m->stages & THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET) {
            for (uint8_t z = 0; z < cfg->zone_count; z++) {
                const thermal_zone_cfg_t *zc = &cfg->zones[z];
                if (!adjusted_trips_valid(zc->trips, zc->trip_count, offset)) {
                    return 0;
                }
                if (zc->governor == THERMAL_GOVERNOR_PID) {
                    int64_t setpoint = (int64_t)zc->pid.setpoint_mc +
                                       (int64_t)offset;
                    if (setpoint < (int64_t)zc->pid.setpoint_min_mc ||
                        setpoint > (int64_t)zc->pid.setpoint_max_mc) {
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

static int runtime_trips_with_modifier_offsets_valid(
        const thermal_core_internal_t *core,
        const thermal_trip_cfg_t *trips,
        uint8_t trip_count) {
    const thermal_config_t *cfg = core->cfg;
    for (uint8_t m = 0; m < cfg->modifier_count; m++) {
        const thermal_modifier_cfg_t *mod = &core->shadow_modifiers[m];
        if (!(mod->stages & THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET)) {
            continue;
        }
        for (uint8_t p = 0; p < mod->curve_count; p++) {
            if (!adjusted_trips_valid(trips, trip_count, mod->curve[p].value1)) {
                return 0;
            }
        }
    }
    return 1;
}

static int runtime_setpoint_with_modifier_offsets_valid(
        const thermal_core_internal_t *core,
        uint16_t zone_id,
        int32_t setpoint_mc) {
    const thermal_config_t *cfg = core->cfg;
    const thermal_pid_cfg_t *pid = &cfg->zones[zone_id].pid;
    for (uint8_t m = 0; m < cfg->modifier_count; m++) {
        const thermal_modifier_cfg_t *mod = &core->shadow_modifiers[m];
        if (!(mod->stages & THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET)) {
            continue;
        }
        for (uint8_t p = 0; p < mod->curve_count; p++) {
            int64_t shifted = (int64_t)setpoint_mc +
                              (int64_t)mod->curve[p].value1;
            if (shifted < (int64_t)pid->setpoint_min_mc ||
                shifted > (int64_t)pid->setpoint_max_mc) {
                return 0;
            }
        }
    }
    return 1;
}

static int modifier_curve_values_valid_runtime(
        const thermal_core_internal_t *core,
        const thermal_modifier_cfg_t *m) {
    const thermal_config_t *cfg = core->cfg;
    for (uint8_t p = 0; p < m->curve_count; p++) {
        int32_t cap = m->curve[p].value0;
        int32_t offset = m->curve[p].value1;
        if (cap < 0 || cap > 255) {
            return 0;
        }
        if (m->stages & THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP) {
            for (uint8_t a = 0; a < cfg->actuator_count; a++) {
                if (cap != 0 && cap < cfg->actuators[a].pwm_min) {
                    return 0;
                }
            }
        }
        if (m->stages & THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET) {
            for (uint8_t z = 0; z < cfg->zone_count; z++) {
                if (!adjusted_trips_valid(core->zones[z].shadow_trips,
                                          cfg->zones[z].trip_count,
                                          offset)) {
                    return 0;
                }
                if (cfg->zones[z].governor == THERMAL_GOVERNOR_PID) {
                    const thermal_pid_cfg_t *pid = &cfg->zones[z].pid;
                    int64_t shifted = (int64_t)core->zones[z].shadow_pid.setpoint_mc +
                                      (int64_t)offset;
                    if (shifted < (int64_t)pid->setpoint_min_mc ||
                        shifted > (int64_t)pid->setpoint_max_mc) {
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

static thermal_status_t validate_modifiers(const thermal_config_t *cfg) {
    for (uint8_t i = 0; i < cfg->modifier_count; i++) {
        const thermal_modifier_cfg_t *m = &cfg->modifiers[i];

        /* Rule 33 (Stage 7 7a, PRD §5.1 line 799). */
        if (strncmp(m->name, "acoustic_mask", THERMAL_NAME_MAX) != 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 34 (PRD §5.3 line 788). */
        if (find_context_slot(cfg, m->context_id) < 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 35 (PRD §5.3 line 793). */
        if (m->curve_count < 2 || m->curve_count > THERMAL_MAX_CURVE_POINTS) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        for (uint8_t k = 1; k < m->curve_count; k++) {
            if (m->curve[k].x <= m->curve[k - 1].x) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
        if (!modifier_curve_values_valid(cfg, m)) {
            return THERMAL_ERR_BOUNDS;
        }
        /* Rule 36 (Stage 7 7c): v1 context cfg has no fallback value field,
         * so ASSUME_VALUE is not implementable. */
        if (m->fail_safe == THERMAL_FAILSAFE_ASSUME_VALUE) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 39 (codex-C): fail_safe must be a known value. */
        if (m->fail_safe != THERMAL_FAILSAFE_ASSUME_STATIONARY &&
            m->fail_safe != THERMAL_FAILSAFE_HOLD_LAST) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 42 (codex-C v2-#9): modifier fail_safe must equal the
         * context's fail_safe. PRD §4.5 describes fail_safe as a
         * context-signal behavior; v1 keeps the modifier field for
         * forward-compat but requires equality at validate-time so
         * tooling/schema can't surprise consumers. */
        {
            int ctx_slot = find_context_slot(cfg, m->context_id);
            if (ctx_slot >= 0 &&
                cfg->contexts[ctx_slot].fail_safe != m->fail_safe) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
        /* Rule 43 (codex-C): stages must be nonzero and contain only
         * known bits (PRE_GOVERNOR_TRIP_OFFSET | POST_GOVERNOR_PWM_CAP). */
        {
            uint8_t known_bits = (uint8_t)(
                THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET |
                THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP);
            if (m->stages == 0 || (m->stages & ~known_bits) != 0) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
    }
    return THERMAL_OK;
}

static thermal_status_t validate_contexts(const thermal_config_t *cfg) {
    for (uint8_t i = 0; i < cfg->context_count; i++) {
        const thermal_context_cfg_t *c = &cfg->contexts[i];
        /* Rule 37 (codex-C): unit must be a known thermal_context_unit_t. */
        if (c->unit != THERMAL_CONTEXT_UNIT_NONE &&
            c->unit != THERMAL_CONTEXT_UNIT_KMH &&
            c->unit != THERMAL_CONTEXT_UNIT_BOOL &&
            c->unit != THERMAL_CONTEXT_UNIT_RPM &&
            c->unit != THERMAL_CONTEXT_UNIT_CELSIUS_MC) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 38 (codex-C): timeout_ms must be > 0 -- stale-context
         * detection depends on it. */
        if (c->timeout_ms == 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 36 (Stage 7 7c): ASSUME_VALUE not implementable in v1. */
        if (c->fail_safe == THERMAL_FAILSAFE_ASSUME_VALUE) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 39 (codex-C): fail_safe must be a known value. */
        if (c->fail_safe != THERMAL_FAILSAFE_ASSUME_STATIONARY &&
            c->fail_safe != THERMAL_FAILSAFE_HOLD_LAST) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* Rule 40 (codex-C v1-#8): IIR alpha must be in [0, Q16_ONE]. */
        if (c->iir_alpha_q16 < 0 || c->iir_alpha_q16 > Q16_ONE) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        for (uint8_t j = (uint8_t)(i + 1); j < cfg->context_count; j++) {
            if (cfg->contexts[i].id == cfg->contexts[j].id) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
    }
    return THERMAL_OK;
}

/* Codex-C v3-#7 / v1-#8 / v2-#6 -- per-detector severity/action enum
 * checks plus rule-46 stuck_sensor correlated context resolution.
 * Each enabled detector kind is validated; disabled ones get their
 * configured fields skipped (a disabled detector with garbage fields
 * is a config smell but not a runtime hazard since the detector
 * never runs). */
static int fault_action_known(uint8_t action) {
    return action == THERMAL_FAULT_ACTION_NONE
        || action == THERMAL_FAULT_ACTION_MARK_DEGRADED
        || action == THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK
        || action == THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED
        || action == THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH
        || action == THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN;
}

static int fault_severity_known(uint8_t sev) {
    return sev == THERMAL_FAULT_SEVERITY_DEGRADED
        || sev == THERMAL_FAULT_SEVERITY_CRITICAL;
}

static thermal_status_t validate_fault_defaults(const thermal_config_t *cfg,
                                                const thermal_fault_detector_cfg_t *fc,
                                                int needs_correlated_ctx_resolution) {
    if (!fc->enabled) return THERMAL_OK;
    /* Rule 44 (codex-C): severity must be known. */
    if (!fault_severity_known(fc->severity)) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    /* Rule 45 (codex-C): action must be known. */
    if (!fault_action_known(fc->action)) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    /* Rule 46 (codex-C v1-#8): stuck_sensor's correlated_context_id
     * (when not advisory 0xFFFF) must resolve to a configured context. */
    if (needs_correlated_ctx_resolution &&
        fc->correlated_context_id != 0xFFFFu &&
        find_context_slot(cfg, fc->correlated_context_id) < 0) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    return THERMAL_OK;
}

static thermal_status_t validate_fault_detectors(const thermal_config_t *cfg) {
    thermal_status_t s;
    if ((s = validate_fault_defaults(cfg, &cfg->faults.stall_defaults, 0))
        != THERMAL_OK) return s;
    if ((s = validate_fault_defaults(cfg, &cfg->faults.stuck_sensor_defaults, 1))
        != THERMAL_OK) return s;
    if ((s = validate_fault_defaults(cfg, &cfg->faults.runaway_defaults, 0))
        != THERMAL_OK) return s;
    if ((s = validate_fault_defaults(cfg, &cfg->faults.stale_context_defaults, 0))
        != THERMAL_OK) return s;
    return THERMAL_OK;
}

/* Rule 47 (codex-C): every (actuator, cooling_state) pair referenced
 * by zone trips must have state_pwm[cs] either 0 or in [pwm_min,
 * pwm_max]. PRD §4.3 line 506 establishes the 0-stays-0 special case;
 * other state_pwm entries that map a trip's cooling_state must be
 * legal PWM commands. Monotonicity is checked only through the highest
 * state a zone can actually request for that actuator; loaders support
 * shorter JSON state_pwm arrays by zero-padding the unused tail. */
static thermal_status_t validate_state_pwm_references(const thermal_config_t *cfg) {
    uint8_t referenced[THERMAL_MAX_ACTUATORS] = { 0 };
    uint8_t max_state[THERMAL_MAX_ACTUATORS] = { 0 };
    for (uint8_t z = 0; z < cfg->zone_count; z++) {
        const thermal_zone_cfg_t *zc = &cfg->zones[z];
        for (uint8_t t = 0; t < zc->trip_count; t++) {
            uint8_t cs = zc->trips[t].cooling_state;
            for (uint8_t k = 0; k < zc->actuator_count; k++) {
                int slot = find_actuator_slot(cfg, zc->actuator_ids[k]);
                if (slot < 0) continue; /* caught elsewhere */
                referenced[slot] = 1;
                if (cs > max_state[slot]) max_state[slot] = cs;
                uint8_t pwm = cfg->actuators[slot].state_pwm[cs];
                if (pwm == 0) continue; /* off is always legal */
                if (pwm < cfg->actuators[slot].pwm_min ||
                    pwm > cfg->actuators[slot].pwm_max) {
                    return THERMAL_ERR_BOUNDS;
                }
            }
        }
    }
    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
        if (!referenced[a]) continue;
        for (uint8_t s = 1; s <= max_state[a]; s++) {
            if (cfg->actuators[a].state_pwm[s] <
                cfg->actuators[a].state_pwm[s - 1]) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
        }
    }
    return THERMAL_OK;
}

/* Rule 48 + Rule 49 (codex-C v1-#8 / v3-#1): telemetry-cfg sanity.
 * - enable=1 with period_ticks=0 silently disables emission today;
 *   reject so the schema can't have a "looks enabled, actually muted"
 *   state.
 * - every enabled_signal_ids[i] must decode to a supported
 *   (range, sub, slot) for the configured counts. */
static thermal_status_t validate_telemetry(const thermal_config_t *cfg) {
    const thermal_telemetry_cfg_t *t = &cfg->telemetry;
    if (t->enable && t->period_ticks == 0) {
        return THERMAL_ERR_INVALID_CONFIG;
    }
    for (uint8_t i = 0; i < t->enabled_signal_count; i++) {
        if (!signal_id_is_supported(cfg, t->enabled_signal_ids[i])) {
            return THERMAL_ERR_INVALID_CONFIG;
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
    if (!zone_fallback_reaches_safety_floor(z, z->trips)) {
        return THERMAL_ERR_INVALID_CONFIG;
    }

    /* PID-zone-specific rules (PRD §5.3 lines 796-798) */
    if (z->governor == THERMAL_GOVERNOR_PID) {
        const thermal_pid_cfg_t *p = &z->pid;
        if (p->kp_q16 < p->kp_min_q16 || p->kp_q16 > p->kp_max_q16) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (p->ki_q16 < p->ki_min_q16 || p->ki_q16 > p->ki_max_q16) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (p->kd_q16 < p->kd_min_q16 || p->kd_q16 > p->kd_max_q16) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (p->setpoint_mc < p->setpoint_min_mc ||
            p->setpoint_mc > p->setpoint_max_mc) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (p->dt_min_ms == 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (p->dt_min_ms > cfg->control_period_ms) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (p->dt_max_ms < cfg->control_period_ms) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
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

/* === Step-helper static functions (Stage 7c) === */

static uint8_t zone_max_severity(const zone_runtime_t *zr,
                                 const thermal_zone_cfg_t *zc) {
    uint8_t max_sev = 0;
    for (uint8_t t = 0; t < zc->trip_count; t++) {
        if (zr->active_trip_mask & ((uint32_t)1u << t)) {
            if (zc->trips[t].severity > max_sev) {
                max_sev = zc->trips[t].severity;
            }
        }
    }
    return max_sev;
}

static uint8_t actuator_max_severity(const thermal_core_internal_t *core,
                                     uint8_t a) {
    const thermal_config_t *cfg = core->cfg;
    uint16_t aid = cfg->actuators[a].id;
    uint8_t max_sev = 0;
    for (uint8_t z = 0; z < cfg->zone_count; z++) {
        const thermal_zone_cfg_t *zc = &cfg->zones[z];
        int referenced = 0;
        for (uint8_t k = 0; k < zc->actuator_count; k++) {
            if (zc->actuator_ids[k] == aid) { referenced = 1; break; }
        }
        if (!referenced) continue;
        uint8_t s = zone_max_severity(&core->zones[z], zc);
        if (s > max_sev) max_sev = s;
    }
    return max_sev;
}

static int action_forces_pwm_max(uint8_t action) {
    return action == THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED
        || action == THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH;
}

static int fault_state_active(uint8_t state) {
    return state != THERMAL_FAULT_NORMAL;
}

static void emit_fault_transition(thermal_core_internal_t *core,
                                  uint8_t fault_type, uint16_t target_id,
                                  uint8_t prev_state, uint8_t new_state,
                                  uint32_t now_ms) {
    if (prev_state == new_state) return;
    uint16_t code = 0;
    if (new_state == THERMAL_FAULT_RECOVERING) {
        code = TEVENT_FAULT_RECOVERING;
    } else if (new_state == THERMAL_FAULT_NORMAL) {
        code = TEVENT_FAULT_CLEAR;
    } else if (prev_state == THERMAL_FAULT_NORMAL) {
        code = TEVENT_FAULT_ENTER;
    } else {
        return; /* internal transition (e.g., DEGRADED -> CRITICAL): skip */
    }
    if (core->cb.log_event) {
        core->cb.log_event(now_ms, code,
                           (uint32_t)fault_type, (uint32_t)target_id,
                           (uint32_t)new_state, 0u);
    }
}

static void step4_context_filters(thermal_core_internal_t *core,
                                  const thermal_input_snapshot_t *in) {
    const thermal_config_t *cfg = core->cfg;
    for (uint8_t c = 0; c < cfg->context_count; c++) {
        int32_t value = 0;
        uint8_t found_valid = 0;
        for (uint8_t s = 0; s < in->sample_count; s++) {
            const thermal_sample_t *smp = &in->samples[s];
            if (smp->kind == THERMAL_SAMPLE_CONTEXT_I32 &&
                smp->id == cfg->contexts[c].id) {
                value = smp->value;
                found_valid = smp->valid;
                break;
            }
        }
        thermal_filter_step(&core->context_filters[c],
                            cfg->contexts[c].iir_alpha_q16,
                            value, found_valid);
        if (found_valid) {
            core->context_last_valid_ms[c] = in->now_ms;
            core->context_ever_valid[c] = 1;
        }
    }
}

static void step5_modifier_eval(thermal_core_internal_t *core) {
    const thermal_config_t *cfg = core->cfg;

    for (uint8_t m = 0; m < THERMAL_MAX_MODIFIERS; m++) {
        core->modifier_active[m] = 0;
        core->modifier_pwm_cap[m] = 0;
        core->modifier_trip_offset[m] = 0;
    }
    for (uint8_t c = 0; c < THERMAL_MAX_CONTEXT_SIGNALS; c++) {
        core->context_fail_safe_active[c] = 0;
        core->context_effective_value[c] = 0;
    }

    if (cfg->modifier_count == 0) return;

    /* Stage 8: read modifier params from shadow (CMD_SET_CURVE_POINT
     * mutates curve[] here; stages/fail_safe/context_id are init-time
     * copies that match cfg). */
    const thermal_modifier_cfg_t *m = &core->shadow_modifiers[0];
    int ctx_slot = find_context_slot(cfg, m->context_id);
    if (ctx_slot < 0) return; /* validate_config rule 34 prevents this */

    /* Codex-C v2-#9 / v3-#3: context-level fail_safe is authoritative.
     * validate_config rule 42 already enforces modifier.fail_safe ==
     * context.fail_safe, so reading from the context is the documented
     * source of truth. */
    const thermal_context_cfg_t *cc = &cfg->contexts[ctx_slot];
    int32_t effective_value;
    uint8_t fail_safe_active;
    if (core->context_filters[ctx_slot].valid) {
        effective_value = core->context_filters[ctx_slot].filtered_value;
        fail_safe_active = 0;
    } else {
        if (cc->fail_safe == THERMAL_FAILSAFE_HOLD_LAST &&
            core->context_ever_valid[ctx_slot]) {
            effective_value = core->context_filters[ctx_slot].filtered_value;
        } else {
            /* STATIONARY (or HOLD_LAST without prior valid sample) -> 0. */
            effective_value = 0;
        }
        fail_safe_active = 1;
    }
    core->context_fail_safe_active[ctx_slot] = fail_safe_active;
    core->context_effective_value[ctx_slot] = effective_value;

    thermal_modifier_eval_result_t r;
    thermal_modifier_eval(m, effective_value, &r);
    core->modifier_pwm_cap[0] = r.pwm_cap;
    core->modifier_trip_offset[0] = r.trip_offset_mc;
    /* Active iff offset shifts trips or cap is below max possible pwm. */
    core->modifier_active[0] = (r.trip_offset_mc != 0) || (r.pwm_cap < 255);
}

/* Returns 1 and writes *out if signal id decodes to a (range, sub, slot)
 * supported by the current config. PRD §7.1 range layout; within-range
 * sub-signal stride is 0x10, slots 0..15 per sub. */
static int dispatch_signal_value(const thermal_core_internal_t *core,
                                 uint16_t sig, int32_t *out) {
    const thermal_config_t *cfg = core->cfg;

    /* Zone range 0x0100..0x01FF */
    if (sig >= TSIG_ZONE_BASE && sig < TSIG_ZONE_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_ZONE_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_ZONE_BASE) & 0x0Fu);
        if (slot >= cfg->zone_count) return 0;
        switch (sub) {
        case TSIG_ZONE_SUB_TEMP:
            *out = core->zones[slot].temp_mc; return 1;
        case TSIG_ZONE_SUB_COOLING_STATE:
            *out = (int32_t)core->zones[slot].cooling_state; return 1;
        case TSIG_ZONE_SUB_TRIP_MASK:
            *out = (int32_t)core->zones[slot].active_trip_mask; return 1;
        case TSIG_ZONE_SUB_EFFECTIVE_SETPOINT:
            *out = core->zones[slot].effective_setpoint_mc; return 1;
        case TSIG_ZONE_SUB_AGGREGATION_VALID:
            *out = (int32_t)core->zones[slot].aggregation_valid; return 1;
        default:
            return 0;
        }
    }
    /* Actuator range 0x0200..0x02FF */
    if (sig >= TSIG_ACTUATOR_BASE && sig < TSIG_ACTUATOR_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_ACTUATOR_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_ACTUATOR_BASE) & 0x0Fu);
        if (slot >= cfg->actuator_count) return 0;
        switch (sub) {
        case TSIG_ACTUATOR_SUB_DUTY:
            *out = (int32_t)core->actuator_prev_duty[slot]; return 1;
        case TSIG_ACTUATOR_SUB_RPM:
            *out = (int32_t)core->actuator_tach_rpm[slot]; return 1;
        case TSIG_ACTUATOR_SUB_SLEW_LIMITED:
            *out = (int32_t)core->actuator_prev_slew_limited[slot]; return 1;
        default:
            return 0;
        }
    }
    /* PID range 0x0300..0x03FF (PRD-locked: zone*4 + term) */
    if (sig >= TSIG_PID_BASE && sig < TSIG_PID_BASE + TSIG_RANGE_SIZE) {
        uint8_t off  = (uint8_t)(sig - TSIG_PID_BASE);
        uint8_t zone = (uint8_t)(off / 4u);
        uint8_t term = (uint8_t)(off % 4u);
        if (zone >= cfg->zone_count) return 0;
        switch (term) {
        case 0: *out = core->zones[zone].pid_error_mc;   return 1;
        case 1: *out = core->zones[zone].pid_i_term_q16; return 1;
        case 2: *out = core->zones[zone].pid_d_term_q16; return 1;
        case 3: *out = (int32_t)core->zones[zone].pwm_this_tick; return 1;
        default: return 0;
        }
    }
    /* Context range 0x0400..0x04FF */
    if (sig >= TSIG_CONTEXT_BASE && sig < TSIG_CONTEXT_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_CONTEXT_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_CONTEXT_BASE) & 0x0Fu);
        if (slot >= cfg->context_count) return 0;
        switch (sub) {
        case TSIG_CONTEXT_SUB_VALUE:
            *out = core->context_effective_value[slot]; return 1;
        case TSIG_CONTEXT_SUB_VALID:
            *out = (int32_t)core->context_filters[slot].valid; return 1;
        default:
            return 0;
        }
    }
    /* Modifier range 0x0500..0x05FF */
    if (sig >= TSIG_MODIFIER_BASE && sig < TSIG_MODIFIER_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_MODIFIER_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_MODIFIER_BASE) & 0x0Fu);
        if (slot >= cfg->modifier_count) return 0;
        switch (sub) {
        case TSIG_MODIFIER_SUB_PWM_CAP:
            *out = core->modifier_pwm_cap[slot]; return 1;
        case TSIG_MODIFIER_SUB_TRIP_OFFSET:
            *out = core->modifier_trip_offset[slot]; return 1;
        case TSIG_MODIFIER_SUB_ACTIVE:
            *out = (int32_t)core->modifier_active[slot]; return 1;
        default:
            return 0;
        }
    }
    /* Fault range 0x0600..0x06FF; each sub uses its detector kind's slot space. */
    if (sig >= TSIG_FAULT_BASE && sig < TSIG_FAULT_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_FAULT_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_FAULT_BASE) & 0x0Fu);
        switch (sub) {
        case TSIG_FAULT_SUB_STALL_STATE:
            if (slot >= cfg->actuator_count) return 0;
            *out = (int32_t)core->stall_states[slot].state; return 1;
        case TSIG_FAULT_SUB_STUCK_SENSOR_STATE:
            if (slot >= cfg->sensor_count) return 0;
            *out = (int32_t)core->stuck_sensor_states[slot].state; return 1;
        case TSIG_FAULT_SUB_RUNAWAY_STATE:
            if (slot >= cfg->zone_count) return 0;
            *out = (int32_t)core->runaway_states[slot].state; return 1;
        case TSIG_FAULT_SUB_STALE_CONTEXT_STATE:
            if (slot >= cfg->context_count) return 0;
            *out = (int32_t)core->stale_context_states[slot].state; return 1;
        default:
            return 0;
        }
    }
#if THERMALCORE_ENABLE_FAN_HEALTH
    /* Fan-health range 0x0800..0x08FF (Stage 17, PRD Appendix C). */
    if (sig >= TSIG_FAN_HEALTH_BASE && sig < TSIG_FAN_HEALTH_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_FAN_HEALTH_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_FAN_HEALTH_BASE) & 0x0Fu);
        if (slot >= cfg->actuator_count) return 0;
        const thermal_fan_health_state_t *fh = &core->fan_health_states[slot];
        switch (sub) {
        case TSIG_FAN_HEALTH_SUB_DELTA:
            *out = (int32_t)fh->health_delta_pct; return 1;
        case TSIG_FAN_HEALTH_SUB_SEVERITY:
            *out = (int32_t)fh->severity; return 1;
        case TSIG_FAN_HEALTH_SUB_BASELINE_SOURCE:
            /* A disabled slot reports `none`, not a zeroed `field`. */
            *out = cfg->fan_health[slot].enable
                       ? (int32_t)cfg->fan_health[slot].baseline_source
                       : (int32_t)THERMAL_FAN_BASELINE_SRC_NONE;
            return 1;
        case TSIG_FAN_HEALTH_SUB_CONFIDENCE:
            *out = (int32_t)fh->confidence; return 1;
        default:
            return 0;
        }
    }
#endif
    return 0;
}

/* Like dispatch_signal_value but cfg-only -- used by validate_config
 * rule 49 to reject telemetry signal IDs that don't decode for the
 * current configured counts. Mirrors the dispatch arithmetic without
 * any state lookup. Returns 1 iff sig is a supported (range, sub,
 * slot) for this cfg. */
static int signal_id_is_supported(const thermal_config_t *cfg, uint16_t sig) {
    if (sig >= TSIG_ZONE_BASE && sig < TSIG_ZONE_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_ZONE_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_ZONE_BASE) & 0x0Fu);
        if (slot >= cfg->zone_count) return 0;
        return (sub == TSIG_ZONE_SUB_TEMP ||
                sub == TSIG_ZONE_SUB_COOLING_STATE ||
                sub == TSIG_ZONE_SUB_TRIP_MASK ||
                sub == TSIG_ZONE_SUB_EFFECTIVE_SETPOINT ||
                sub == TSIG_ZONE_SUB_AGGREGATION_VALID);
    }
    if (sig >= TSIG_ACTUATOR_BASE && sig < TSIG_ACTUATOR_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_ACTUATOR_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_ACTUATOR_BASE) & 0x0Fu);
        if (slot >= cfg->actuator_count) return 0;
        return (sub == TSIG_ACTUATOR_SUB_DUTY ||
                sub == TSIG_ACTUATOR_SUB_RPM ||
                sub == TSIG_ACTUATOR_SUB_SLEW_LIMITED);
    }
    if (sig >= TSIG_PID_BASE && sig < TSIG_PID_BASE + TSIG_RANGE_SIZE) {
        uint8_t off  = (uint8_t)(sig - TSIG_PID_BASE);
        uint8_t zone = (uint8_t)(off / 4u);
        return zone < cfg->zone_count;
    }
    if (sig >= TSIG_CONTEXT_BASE && sig < TSIG_CONTEXT_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_CONTEXT_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_CONTEXT_BASE) & 0x0Fu);
        if (slot >= cfg->context_count) return 0;
        return (sub == TSIG_CONTEXT_SUB_VALUE ||
                sub == TSIG_CONTEXT_SUB_VALID);
    }
    if (sig >= TSIG_MODIFIER_BASE && sig < TSIG_MODIFIER_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_MODIFIER_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_MODIFIER_BASE) & 0x0Fu);
        if (slot >= cfg->modifier_count) return 0;
        return (sub == TSIG_MODIFIER_SUB_PWM_CAP ||
                sub == TSIG_MODIFIER_SUB_TRIP_OFFSET ||
                sub == TSIG_MODIFIER_SUB_ACTIVE);
    }
    if (sig >= TSIG_FAULT_BASE && sig < TSIG_FAULT_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_FAULT_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_FAULT_BASE) & 0x0Fu);
        switch (sub) {
        case TSIG_FAULT_SUB_STALL_STATE:         return slot < cfg->actuator_count;
        case TSIG_FAULT_SUB_STUCK_SENSOR_STATE:  return slot < cfg->sensor_count;
        case TSIG_FAULT_SUB_RUNAWAY_STATE:       return slot < cfg->zone_count;
        case TSIG_FAULT_SUB_STALE_CONTEXT_STATE: return slot < cfg->context_count;
        default: return 0;
        }
    }
#if THERMALCORE_ENABLE_FAN_HEALTH
    if (sig >= TSIG_FAN_HEALTH_BASE && sig < TSIG_FAN_HEALTH_BASE + TSIG_RANGE_SIZE) {
        uint8_t sub  = (uint8_t)((sig - TSIG_FAN_HEALTH_BASE) & 0xF0u);
        uint8_t slot = (uint8_t)((sig - TSIG_FAN_HEALTH_BASE) & 0x0Fu);
        if (slot >= cfg->actuator_count) return 0;
        return (sub == TSIG_FAN_HEALTH_SUB_DELTA ||
                sub == TSIG_FAN_HEALTH_SUB_SEVERITY ||
                sub == TSIG_FAN_HEALTH_SUB_BASELINE_SOURCE ||
                sub == TSIG_FAN_HEALTH_SUB_CONFIDENCE);
    }
#endif
    return 0;
}

/* === Public API === */

#if THERMALCORE_ENABLE_FAN_HEALTH
/* Stage 17 (PRD Appendix C): semantic validation of the per-actuator
 * fan-health baseline. The Linux loader and json2static.py do the
 * structural parsing; the monotonicity / ordering / count invariants
 * live here so a hand-built thermal_config_t is caught too. */
static thermal_status_t validate_fan_health(const thermal_config_t *cfg) {
    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
        const thermal_fan_health_cfg_t *fh = &cfg->fan_health[a];
        if (!fh->enable) continue;
        if (fh->baseline_source > THERMAL_FAN_BASELINE_SRC_MODEL) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (fh->stable_rpm_tolerance_pct > 100) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (fh->baseline_count < 2 ||
            fh->baseline_count > THERMAL_MAX_FAN_HEALTH_POINTS) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        /* The baseline must reach the actuator's pwm_max so high-duty
         * operation is never scored against a clamped expected RPM. */
        if (fh->baseline[fh->baseline_count - 1].x < cfg->actuators[a].pwm_max) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        for (uint8_t p = 0; p < fh->baseline_count; p++) {
            if (fh->baseline[p].x < 0 || fh->baseline[p].x > 255) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
            if (fh->baseline[p].value0 < 1 || fh->baseline[p].value0 > 65535) {
                return THERMAL_ERR_INVALID_CONFIG;
            }
            if (p > 0) {
                /* PWM strictly ascending; RPM monotonic non-decreasing. */
                if (fh->baseline[p].x <= fh->baseline[p - 1].x) {
                    return THERMAL_ERR_INVALID_CONFIG;
                }
                if (fh->baseline[p].value0 < fh->baseline[p - 1].value0) {
                    return THERMAL_ERR_INVALID_CONFIG;
                }
            }
        }
        /* Signed severity thresholds: 0 > aging > degraded > failing. */
        if (!(0 > fh->aging_pct &&
              fh->aging_pct > fh->degraded_pct &&
              fh->degraded_pct > fh->failing_pct)) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (fh->stable_pwm_ticks == 0 || fh->stable_rpm_ticks == 0) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
        if (fh->min_points_observed < 1 ||
            fh->min_points_observed > fh->baseline_count) {
            return THERMAL_ERR_INVALID_CONFIG;
        }
    }
    return THERMAL_OK;
}
#endif /* THERMALCORE_ENABLE_FAN_HEALTH */

thermal_status_t thermal_core_validate_config(const thermal_config_t *cfg) {
    if (!cfg) return THERMAL_ERR_INVALID_ARG;
    if (cfg->config_version != 1) return THERMAL_ERR_INVALID_CONFIG;
    if (cfg->control_period_ms == 0) return THERMAL_ERR_INVALID_CONFIG;
    if (cfg->period_relative_to_ms != 0 &&
        cfg->period_relative_to_ms != cfg->control_period_ms) {
        return THERMAL_ERR_INVALID_CONFIG;
    }

    if (cfg->sensor_count   > THERMAL_MAX_SENSORS)         return THERMAL_ERR_NO_SPACE;
    if (cfg->zone_count     > THERMAL_MAX_ZONES)           return THERMAL_ERR_NO_SPACE;
    if (cfg->actuator_count > THERMAL_MAX_ACTUATORS)       return THERMAL_ERR_NO_SPACE;
    if (cfg->context_count  > THERMAL_MAX_CONTEXT_SIGNALS) return THERMAL_ERR_NO_SPACE;
    if (cfg->modifier_count > THERMAL_MAX_MODIFIERS)       return THERMAL_ERR_NO_SPACE;
    if (cfg->telemetry.enabled_signal_count > THERMAL_MAX_TELEMETRY_SIGNALS) {
        return THERMAL_ERR_NO_SPACE;
    }

    if (cfg->modifier_count > 1) return THERMAL_ERR_INVALID_CONFIG;

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

    if ((s = validate_modifiers(cfg)) != THERMAL_OK) return s;

    /* Codex-C v1-#8 / v2-#6 / v3-#2 additions. */
    if ((s = validate_fault_detectors(cfg)) != THERMAL_OK) return s;
    if ((s = validate_state_pwm_references(cfg)) != THERMAL_OK) return s;
    if ((s = validate_telemetry(cfg)) != THERMAL_OK) return s;

#if THERMALCORE_ENABLE_FAN_HEALTH
    if ((s = validate_fan_health(cfg)) != THERMAL_OK) return s;
#endif

    return THERMAL_OK;
}

thermal_status_t thermal_core_init(thermal_core_t *ctx,
                                   const thermal_config_t *cfg,
                                   const thermal_core_callbacks_t *cb) {
    if (!ctx || !cfg || !cb) return THERMAL_ERR_INVALID_ARG;

    thermal_status_t s = thermal_core_validate_config(cfg);
    if (s != THERMAL_OK) return s;

    thermal_core_internal_t *core = (thermal_core_internal_t *)ctx;
    memset(core, 0, sizeof(*core));
    core->cfg = cfg;
    core->cb = *cb;

    for (uint8_t i = 0; i < THERMAL_MAX_SENSORS; i++) {
        thermal_filter_reset(&core->filters[i]);
    }
    for (uint8_t i = 0; i < THERMAL_MAX_CONTEXT_SIGNALS; i++) {
        thermal_filter_reset(&core->context_filters[i]);
    }

    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        zone_runtime_t *zr = &core->zones[i];
        const thermal_zone_cfg_t *zc = &cfg->zones[i];
        zr->sensor_slot_count = zc->sensor_count;
        for (uint8_t k = 0; k < zc->sensor_count; k++) {
            zr->sensor_slots[k] = (uint8_t)find_sensor_slot(cfg, zc->sensor_ids[k]);
        }
        thermal_pid_reset(&zr->pid);
        /* Stage 8: shadow cfg sub-structs that commands can mutate. */
        zr->shadow_pid = zc->pid;
        for (uint8_t t = 0; t < zc->trip_count; t++) {
            zr->shadow_trips[t] = zc->trips[t];
        }
    }

    /* Stage 8: shadow modifiers (CMD_SET_CURVE_POINT target). */
    for (uint8_t i = 0; i < cfg->modifier_count; i++) {
        core->shadow_modifiers[i] = cfg->modifiers[i];
    }

    for (uint8_t i = 0; i < THERMAL_MAX_ACTUATORS; i++) {
        thermal_fault_stall_reset(&core->stall_states[i]);
        if (i < cfg->actuator_count) {
            core->actuator_off_ticks[i] = cfg->actuators[i].min_off_ticks;
        }
    }
    for (uint8_t i = 0; i < THERMAL_MAX_SENSORS; i++) {
        thermal_fault_stuck_sensor_reset(&core->stuck_sensor_states[i]);
    }
    for (uint8_t i = 0; i < THERMAL_MAX_ZONES; i++) {
        thermal_fault_runaway_reset(&core->runaway_states[i]);
    }
    for (uint8_t i = 0; i < THERMAL_MAX_CONTEXT_SIGNALS; i++) {
        thermal_fault_stale_context_reset(&core->stale_context_states[i]);
    }
#if THERMALCORE_ENABLE_FAN_HEALTH
    for (uint8_t i = 0; i < THERMAL_MAX_ACTUATORS; i++) {
        thermal_fan_health_reset(&core->fan_health_states[i]);
    }
#endif

    return THERMAL_OK;
}

/* Snapshot preflight per PRD §4.3 line 494: reject malformed snapshots
 * before any state mutation. Detects oversized, NULL+nonzero, unknown
 * kind, unknown (kind,id) target, and duplicate (kind,id) within the
 * snapshot. Returns the documented status code; callers must not
 * mutate state when this fails. */
static thermal_status_t preflight_snapshot(const thermal_config_t *cfg,
                                           const thermal_input_snapshot_t *in) {
    if (in->sample_count > THERMAL_MAX_SAMPLES_PER_SNAPSHOT) {
        return THERMAL_ERR_NO_SPACE;
    }
    if (in->sample_count > 0 && in->samples == NULL) {
        return THERMAL_ERR_INVALID_ARG;
    }
    /* Pack (kind, slot) into uint16 (kind in high 8 bits, slot in low 8;
     * slot < 256 since v1 maxima never approach that). Detect duplicates
     * via linear scan against earlier entries -- bounded by
     * THERMAL_MAX_SAMPLES_PER_SNAPSHOT so it stays O(n^2) of a small n. */
    uint16_t seen[THERMAL_MAX_SAMPLES_PER_SNAPSHOT];
    uint8_t  seen_count = 0;
    for (uint8_t s = 0; s < in->sample_count; s++) {
        const thermal_sample_t *smp = &in->samples[s];
        int slot;
        switch (smp->kind) {
        case THERMAL_SAMPLE_TEMP_MC:
            slot = find_sensor_slot(cfg, smp->id);
            break;
        case THERMAL_SAMPLE_TACH_RPM:
            slot = find_actuator_slot(cfg, smp->id);
            break;
        case THERMAL_SAMPLE_CONTEXT_I32:
            slot = find_context_slot(cfg, smp->id);
            break;
        default:
            return THERMAL_ERR_INVALID_ARG;
        }
        if (slot < 0) return THERMAL_ERR_INVALID_ARG;
        uint16_t key = (uint16_t)(((uint16_t)smp->kind << 8) | (uint16_t)slot);
        for (uint8_t k = 0; k < seen_count; k++) {
            if (seen[k] == key) return THERMAL_ERR_INVALID_ARG;
        }
        seen[seen_count++] = key;
    }
    return THERMAL_OK;
}

thermal_status_t thermal_core_step(thermal_core_t *ctx,
                                   const thermal_input_snapshot_t *in,
                                   thermal_output_frame_t *out) {
    if (!ctx || !in || !out) return THERMAL_ERR_INVALID_ARG;
    thermal_core_internal_t *core = (thermal_core_internal_t *)ctx;
    if (core->cfg == NULL) return THERMAL_ERR_STATE;
    const thermal_config_t *cfg = core->cfg;

    /* === Snapshot preflight (PRD §4.3 line 494): reject malformed
     * snapshots before mutating any state. */
    thermal_status_t pf = preflight_snapshot(cfg, in);
    if (pf != THERMAL_OK) return pf;

    /* === §4.6 step 3: sensor IIR filters.
     * Walk every CONFIGURED sensor (not just samples) so absent sensors
     * call thermal_filter_step with sample_valid=0, clearing valid while
     * preserving filtered_value per PRD §4.5 filter lifecycle. */
    for (uint8_t s = 0; s < cfg->sensor_count; s++) {
        int32_t value = 0;
        uint8_t found_valid = 0;
        for (uint8_t k = 0; k < in->sample_count; k++) {
            const thermal_sample_t *smp = &in->samples[k];
            if (smp->kind == THERMAL_SAMPLE_TEMP_MC &&
                smp->id == cfg->sensors[s].id) {
                value = smp->value;
                found_valid = smp->valid;
                break;
            }
        }
        thermal_filter_step(&core->filters[s],
                            cfg->sensors[s].iir_alpha_q16,
                            value, found_valid);
    }

    /* === Step 3b: per-actuator tach pickup from THERMAL_SAMPLE_TACH_RPM.
     * Preflight already validated id-as-actuator. Step 9's stall detector
     * reads these; get_state reports them via thermal_actuator_state_t. */
    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
        uint16_t rpm = 0;
        uint8_t  found_valid = 0;
        for (uint8_t k = 0; k < in->sample_count; k++) {
            const thermal_sample_t *smp = &in->samples[k];
            if (smp->kind == THERMAL_SAMPLE_TACH_RPM &&
                smp->id == cfg->actuators[a].id) {
                int32_t v = smp->value;
                if (v < 0) v = 0;
                if (v > 0xFFFF) v = 0xFFFF;
                rpm = (uint16_t)v;
                found_valid = smp->valid;
                break;
            }
        }
        core->actuator_tach_rpm[a] = rpm;
        core->actuator_tach_valid[a] = found_valid;
    }

    /* === §4.6 step 4: context filters + staleness === */
    step4_context_filters(core, in);

    /* === §4.6 step 5: pre-governor modifier eval === */
    step5_modifier_eval(core);

    /* === Stuck-sensor pre-pass (moved out of step 9). Runs against the
     * just-filtered sensor values so USE_ZONE_FALLBACK action can swap
     * a zone's temperature for fallback_temp_mc BEFORE the governor
     * sees it (PRD §4.7 use_zone_fallback). Event emission stays here
     * to keep transitions in deterministic order. */
    /* Codex-v3 #4: per-zone bitmask of stuck sensor slots (within the
     * zone's sensor_ids[] indexing). Step 6 aggregation masks these to
     * invalid; aggregation naturally computes "max of remaining valid
     * sensors" and falls back to fallback_temp_mc only when every
     * sensor in the zone is masked (PRD §4.7 use_zone_fallback). */
    uint8_t zone_stuck_mask[THERMAL_MAX_ZONES] = { 0 };
    if (cfg->faults.stuck_sensor_defaults.enabled) {
        const thermal_fault_detector_cfg_t *fc = &cfg->faults.stuck_sensor_defaults;
        /* Derive correlated_load_changing once per tick from the
         * configured correlated context's value delta. The detector
         * accumulates this boolean across its sensor-flatness window, so
         * a single load/context transition inside the window is enough
         * to make a flat sensor actionable. With 0xFFFF, the detector
         * runs in flatness-only mode. */
        uint8_t correlated_load_changing = 0;
        if (fc->correlated_context_id != 0xFFFF) {
            int ctx_slot = find_context_slot(cfg, fc->correlated_context_id);
            if (ctx_slot >= 0) {
                uint8_t curr_valid = core->context_filters[ctx_slot].valid;
                int32_t curr_value = core->context_filters[ctx_slot].filtered_value;
                if (curr_valid &&
                    core->stuck_context_prev_valid[ctx_slot] &&
                    curr_value != core->stuck_context_prev_value[ctx_slot]) {
                    correlated_load_changing = 1;
                }
            }
        }
        for (uint8_t s = 0; s < cfg->sensor_count; s++) {
            uint8_t prev = core->stuck_sensor_states[s].state;
            thermal_fault_stuck_sensor_step(&core->stuck_sensor_states[s], fc,
                                            core->filters[s].filtered_value,
                                            core->filters[s].valid,
                                            correlated_load_changing,
                                            in->now_ms);
            emit_fault_transition(core, THERMAL_FAULT_TYPE_STUCK_SENSOR,
                                  cfg->sensors[s].id, prev,
                                  core->stuck_sensor_states[s].state,
                                  in->now_ms);
            /* Codex-v3 #6: emit TEVENT_SHUTDOWN_REQUEST once per detector
             * rising edge when action == REQUEST_SHUTDOWN. The force-max +
             * latch path runs later in step 9; the event itself belongs
             * here next to the other detector rising-edge emissions. */
            if (prev == THERMAL_FAULT_NORMAL
                && core->stuck_sensor_states[s].state != THERMAL_FAULT_NORMAL
                && fc->action == THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN
                && core->cb.log_event) {
                core->cb.log_event(in->now_ms, TEVENT_SHUTDOWN_REQUEST,
                                   (uint32_t)cfg->sensors[s].id,
                                   (uint32_t)THERMAL_FAULT_TYPE_STUCK_SENSOR,
                                   0u, 0u);
            }
            if (fault_state_active(core->stuck_sensor_states[s].state)
                && fc->action == THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK) {
                /* For each zone referencing this sensor, set the
                 * within-zone slot bit in the stuck mask. Step 6's
                 * aggregation will treat that slot as invalid; the
                 * zone falls back to fallback_temp_mc only if every
                 * referencing sensor is masked. */
                for (uint8_t z = 0; z < cfg->zone_count; z++) {
                    const thermal_zone_cfg_t *zc = &cfg->zones[z];
                    for (uint8_t k = 0; k < zc->sensor_count; k++) {
                        if (zc->sensor_ids[k] == cfg->sensors[s].id) {
                            zone_stuck_mask[z] |= (uint8_t)(1u << k);
                            break;
                        }
                    }
                }
            }
        }
        if (fc->correlated_context_id != 0xFFFF) {
            int ctx_slot = find_context_slot(cfg, fc->correlated_context_id);
            if (ctx_slot >= 0) {
                core->stuck_context_prev_value[ctx_slot] =
                    core->context_filters[ctx_slot].filtered_value;
                core->stuck_context_prev_valid[ctx_slot] =
                    core->context_filters[ctx_slot].valid;
            }
        }
    }

    /* === §4.6 step 6: per-zone aggregate + governor (offset-adjusted trips) === */
    int32_t  filter_values[THERMAL_MAX_SENSORS_PER_ZONE];
    uint8_t  filter_valid [THERMAL_MAX_SENSORS_PER_ZONE];
    int32_t  weights      [THERMAL_MAX_SENSORS_PER_ZONE];
    int32_t  trip_offset_mc = 0;
    if (cfg->modifier_count > 0 && core->modifier_active[0]
        && (core->shadow_modifiers[0].stages &
            THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET)) {
        trip_offset_mc = core->modifier_trip_offset[0];
    }

    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        zone_runtime_t *zr = &core->zones[i];
        const thermal_zone_cfg_t *zc = &cfg->zones[i];

        for (uint8_t k = 0; k < zc->sensor_count; k++) {
            uint8_t slot = zr->sensor_slots[k];
            filter_values[k] = core->filters[slot].filtered_value;
            filter_valid[k]  = core->filters[slot].valid;
            /* Codex-v3 #4: mask out stuck sensors so aggregation
             * computes the max of remaining valid sensors. If every
             * slot is masked, thermal_zone_aggregate falls back to
             * fallback_temp_mc per its existing contract. */
            if (zone_stuck_mask[i] & (uint8_t)(1u << k)) {
                filter_valid[k] = 0;
            }
            weights[k]       = zc->sensor_weights_q16[k];
        }
        thermal_zone_aggregate_result_t agg;
        thermal_zone_aggregate((thermal_aggregation_t)zc->aggregation,
                               filter_values, filter_valid, weights,
                               zc->sensor_count, zc->fallback_temp_mc, &agg);
        zr->temp_mc = agg.temp_mc;
        if ((!zr->aggregation_seen && !agg.valid) ||
            (zr->aggregation_seen && zr->aggregation_valid != agg.valid)) {
            if (core->cb.log_event) {
                uint16_t code = agg.valid ? TEVENT_ZONE_FALLBACK_EXIT
                                          : TEVENT_ZONE_FALLBACK_ENTER;
                core->cb.log_event(in->now_ms, code, (uint32_t)i,
                                   (uint32_t)zc->fallback_temp_mc, 0u, 0u);
            }
        }
        zr->aggregation_valid = agg.valid;
        zr->aggregation_seen = 1;

        /* Build offset-adjusted trip array if modifier active. Trip
         * temp/hyst come from shadow (CMD_SET_TRIP target); severity
         * and cooling_state are init-time copies matching cfg. */
        thermal_trip_cfg_t adj_trips[THERMAL_MAX_TRIPS_PER_ZONE];
        const thermal_trip_cfg_t *trips_eff = zr->shadow_trips;
        if (trip_offset_mc != 0) {
            for (uint8_t t = 0; t < zc->trip_count; t++) {
                adj_trips[t] = zr->shadow_trips[t];
                adj_trips[t].temp_mc += trip_offset_mc;
            }
            trips_eff = adj_trips;
        }

        if (zc->governor == THERMAL_GOVERNOR_STEP_WISE) {
            thermal_governor_step_result_t gr;
            /* Use zr->temp_mc (post-fallback) so USE_ZONE_FALLBACK actually
             * drives control; codex-v2 #2 bug fix. */
            thermal_governor_step_wise(zr->temp_mc, trips_eff, zc->trip_count,
                                       zr->active_trip_mask, &gr);
            zr->active_trip_mask = gr.active_trip_mask;
            zr->cooling_state    = gr.cooling_state;
            zr->pwm_this_tick    = 0; /* step 7 uses state_pwm[cooling_state] */
            zr->effective_setpoint_mc = 0;
            zr->pid_error_mc = 0;
            zr->pid_p_term_q16 = 0;
            zr->pid_i_term_q16 = 0;
            zr->pid_d_term_q16 = 0;
        } else {
            /* Stage 8: PID gains + setpoint come from shadow
             * (CMD_SET_PID / CMD_SET_SETPOINT targets). dt bounds are
             * init-time copies. */
            int32_t setpoint_eff = zr->shadow_pid.setpoint_mc + trip_offset_mc;
            /* Use zr->temp_mc (post-fallback) so USE_ZONE_FALLBACK drives
             * the PID; codex-v2 #2 bug fix. */
            thermal_pid_step_result_t pr;
            thermal_pid_step(&zr->pid,
                             zr->shadow_pid.kp_q16,
                             zr->shadow_pid.ki_q16,
                             zr->shadow_pid.kd_q16,
                             setpoint_eff, zr->temp_mc,
                             in->now_ms,
                             zr->shadow_pid.dt_min_ms,
                             zr->shadow_pid.dt_max_ms,
                             0, 255,
                             &pr);
            zr->pwm_this_tick = (uint8_t)pr.output_pwm;
            zr->effective_setpoint_mc = setpoint_eff;
            zr->pid_error_mc   = zr->temp_mc - setpoint_eff;
            zr->pid_p_term_q16 = pr.p_term_q16;
            zr->pid_i_term_q16 = pr.i_term_q16;
            zr->pid_d_term_q16 = pr.d_term_q16;

            /* PID safety floor via step-wise on the same offset-adjusted
             * trips (PRD §4.8 line 670): cooling_state for the snapshot
             * reflects only CRITICAL/SHUTDOWN trips. */
            thermal_governor_step_result_t gr;
            thermal_governor_step_wise(zr->temp_mc, trips_eff, zc->trip_count,
                                       zr->active_trip_mask, &gr);
            zr->active_trip_mask = gr.active_trip_mask;
            uint8_t pid_cs = 0;
            for (uint8_t k = 0; k < zc->trip_count; k++) {
                if (!(gr.active_trip_mask & ((uint32_t)1u << k))) continue;
                const thermal_trip_cfg_t *t = &trips_eff[k];
                if (t->severity != THERMAL_TRIP_CRITICAL &&
                    t->severity != THERMAL_TRIP_SHUTDOWN) continue;
                if (t->cooling_state > pid_cs) pid_cs = t->cooling_state;
            }
            zr->cooling_state = pid_cs;
        }
    }

    /* === §4.6 step 7: per-actuator max-wins arbitration === */
    uint8_t arb_pwm[THERMAL_MAX_ACTUATORS];
    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
        uint16_t aid = cfg->actuators[a].id;
        uint8_t demands[THERMAL_MAX_ZONES];
        uint8_t count = 0;
        for (uint8_t z = 0; z < cfg->zone_count; z++) {
            const thermal_zone_cfg_t *zc = &cfg->zones[z];
            int referenced = 0;
            for (uint8_t k = 0; k < zc->actuator_count; k++) {
                if (zc->actuator_ids[k] == aid) { referenced = 1; break; }
            }
            if (!referenced) continue;
            uint8_t d;
            if (zc->governor == THERMAL_GOVERNOR_STEP_WISE) {
                d = cfg->actuators[a].state_pwm[core->zones[z].cooling_state];
            } else {
                /* PRD §4.7 trip severity table: PID zones floor PID output
                 * to state_pwm[critical_floor_cs]. zone_runtime.cooling_state
                 * was computed in step 6 as max cooling_state over active
                 * CRITICAL/SHUTDOWN trips (0 if none). */
                uint8_t pid_d = core->zones[z].pwm_this_tick;
                uint8_t floor = cfg->actuators[a].state_pwm[core->zones[z].cooling_state];
                d = (floor > pid_d) ? floor : pid_d;
            }
            demands[count++] = d;
        }
        arb_pwm[a] = thermal_arbitrator_max_wins(demands, count);
        core->actuator_prev_requested[a] = arb_pwm[a];
    }

    /* === §4.6 step 8: post-governor modifier pwm_cap (skip if CRITICAL+) === */
    uint8_t actuator_max_zone_sev[THERMAL_MAX_ACTUATORS];
    uint8_t modifier_cap_applied[THERMAL_MAX_ACTUATORS] = { 0 };
    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
        actuator_max_zone_sev[a] = actuator_max_severity(core, a);
        if (actuator_max_zone_sev[a] >= THERMAL_TRIP_CRITICAL) continue;
        if (cfg->modifier_count > 0 && core->modifier_active[0]
            && (cfg->modifiers[0].stages &
                THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP)) {
            int32_t cap = core->modifier_pwm_cap[0];
            if (cap >= 0 && cap < (int32_t)cfg->actuators[a].pwm_max) {
                if ((uint8_t)cap < arb_pwm[a]) {
                    arb_pwm[a] = (uint8_t)cap;
                    modifier_cap_applied[a] = 1;
                }
            }
        }
    }

    /* === §4.6 step 9: fault detectors + flag derivation.
     *
     * Three distinct per-actuator flags replace the conflated "safety
     * override" of earlier revisions:
     *   slew_override_up[a]   -- bypass slew upward; set on CRITICAL+
     *                            severity or any active fault with a
     *                            force_pwm_max/request_shutdown action
     *                            affecting this actuator.
     *   force_pwm_max[a]      -- overwrite arb_pwm to actuator.pwm_max;
     *                            set on SHUTDOWN severity or any active
     *                            fault with a force_pwm_max/request_shutdown
     *                            action. CRITICAL alone does NOT force max
     *                            (PRD §4.7 trip severity table).
     *   shutdown_action[a]    -- set when a fault action == REQUEST_SHUTDOWN
     *                            triggers shutdown for this actuator.
     *
     * Cause tracking lets step 10 produce the right reason code. */
    uint8_t slew_override_up[THERMAL_MAX_ACTUATORS] = { 0 };
    uint8_t force_pwm_max[THERMAL_MAX_ACTUATORS]    = { 0 };
    uint8_t shutdown_action[THERMAL_MAX_ACTUATORS]  = { 0 };
    uint8_t cause_stall[THERMAL_MAX_ACTUATORS]      = { 0 };
    uint8_t cause_runaway[THERMAL_MAX_ACTUATORS]    = { 0 };
    uint8_t cause_stuck[THERMAL_MAX_ACTUATORS]      = { 0 };
    uint8_t cause_stale[THERMAL_MAX_ACTUATORS]      = { 0 };

    /* Severity-derived flags. */
    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
        if (actuator_max_zone_sev[a] >= THERMAL_TRIP_CRITICAL) {
            slew_override_up[a] = 1;
        }
        if (actuator_max_zone_sev[a] >= THERMAL_TRIP_SHUTDOWN) {
            force_pwm_max[a] = 1;
        }
    }

    /* Stall (per actuator). Tach is now plumbed (Stage 8.5 codex-B):
     * step 3b populated actuator_tach_rpm / actuator_tach_valid from
     * THERMAL_SAMPLE_TACH_RPM samples. */
    if (cfg->faults.stall_defaults.enabled) {
        const thermal_fault_detector_cfg_t *fc = &cfg->faults.stall_defaults;
        for (uint8_t a = 0; a < cfg->actuator_count; a++) {
            uint16_t spinup_ticks = (cfg->control_period_ms > 0)
                ? (uint16_t)((cfg->actuators[a].spinup_ms +
                              cfg->control_period_ms - 1) /
                             cfg->control_period_ms)
                : 0;
            uint8_t prev = core->stall_states[a].state;
            thermal_fault_stall_step(&core->stall_states[a], fc,
                                     arb_pwm[a],
                                     core->actuator_tach_rpm[a],
                                     core->actuator_tach_valid[a],
                                     cfg->actuators[a].spinup_pwm,
                                     spinup_ticks, in->now_ms);
            emit_fault_transition(core, THERMAL_FAULT_TYPE_STALL,
                                  cfg->actuators[a].id, prev,
                                  core->stall_states[a].state, in->now_ms);
            if (fault_state_active(core->stall_states[a].state)) {
                if (action_forces_pwm_max(fc->action)) {
                    slew_override_up[a] = 1;
                    force_pwm_max[a] = 1;
                    cause_stall[a] = 1;
                } else if (fc->action == THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN) {
                    slew_override_up[a] = 1;
                    force_pwm_max[a] = 1;
                    shutdown_action[a] = 1;
                    /* Rising edge of detector entering non-NORMAL state. */
                    if (prev == THERMAL_FAULT_NORMAL && core->cb.log_event) {
                        core->cb.log_event(in->now_ms, TEVENT_SHUTDOWN_REQUEST,
                                           (uint32_t)cfg->actuators[a].id,
                                           (uint32_t)THERMAL_FAULT_TYPE_STALL,
                                           0u, 0u);
                    }
                }
            }
        }
    }

    /* Runaway (per zone). commanded_pwm is the max arb across this zone's
     * actuators. */
    if (cfg->faults.runaway_defaults.enabled) {
        const thermal_fault_detector_cfg_t *fc = &cfg->faults.runaway_defaults;
        for (uint8_t z = 0; z < cfg->zone_count; z++) {
            uint8_t rep_pwm = 0;
            for (uint8_t k = 0; k < cfg->zones[z].actuator_count; k++) {
                int slot = find_actuator_slot(cfg, cfg->zones[z].actuator_ids[k]);
                if (slot >= 0 && arb_pwm[slot] > rep_pwm) rep_pwm = arb_pwm[slot];
            }
            uint8_t prev = core->runaway_states[z].state;
            thermal_fault_runaway_step(&core->runaway_states[z], fc,
                                       core->zones[z].temp_mc, rep_pwm,
                                       in->now_ms);
            emit_fault_transition(core, THERMAL_FAULT_TYPE_RUNAWAY,
                                  z, prev, core->runaway_states[z].state,
                                  in->now_ms);
            if (fault_state_active(core->runaway_states[z].state)) {
                int forces_max = action_forces_pwm_max(fc->action);
                int requests_sd = (fc->action == THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN);
                int escalates_sd = 0;
                if (fc->action == THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH &&
                    core->runaway_states[z].state == THERMAL_FAULT_LATCHED) {
                    if (core->runaway_states[z].recovery_count == 0) {
                        if (core->runaway_latched_active_ticks[z] < 0xFFFF) {
                            core->runaway_latched_active_ticks[z]++;
                        }
                    } else {
                        core->runaway_latched_active_ticks[z] = 0;
                    }
                    if (fc->recovery_ticks > 0 &&
                        core->runaway_latched_active_ticks[z] >= fc->recovery_ticks) {
                        escalates_sd = 1;
                    }
                } else {
                    core->runaway_latched_active_ticks[z] = 0;
                }
                if (forces_max || requests_sd || escalates_sd) {
                    for (uint8_t k = 0; k < cfg->zones[z].actuator_count; k++) {
                        int slot = find_actuator_slot(cfg,
                            cfg->zones[z].actuator_ids[k]);
                        if (slot < 0) continue;
                        slew_override_up[slot] = 1;
                        force_pwm_max[slot] = 1;
                        if (escalates_sd) {
                            shutdown_action[slot] = 1;
                        } else if (forces_max) {
                            cause_runaway[slot] = 1;
                        } else {
                            shutdown_action[slot] = 1;
                        }
                    }
                    /* Rising edge: emit shutdown event once per detector. */
                    if (requests_sd && prev == THERMAL_FAULT_NORMAL
                        && core->cb.log_event) {
                        core->cb.log_event(in->now_ms, TEVENT_SHUTDOWN_REQUEST,
                                           (uint32_t)z,
                                           (uint32_t)THERMAL_FAULT_TYPE_RUNAWAY,
                                           0u, 0u);
                    }
                    if (escalates_sd &&
                        !core->runaway_escalated_shutdown[z] &&
                        core->cb.log_event) {
                        core->cb.log_event(in->now_ms, TEVENT_SHUTDOWN_REQUEST,
                                           (uint32_t)z,
                                           (uint32_t)THERMAL_FAULT_TYPE_RUNAWAY,
                                           (uint32_t)THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH,
                                           0u);
                        core->runaway_escalated_shutdown[z] = 1;
                    }
                }
            } else {
                core->runaway_latched_active_ticks[z] = 0;
                core->runaway_escalated_shutdown[z] = 0;
            }
        }
    }

    /* Stale context (per context). Detector emits events; v1 codex-B
     * extends action dispatch to honor FORCE_PWM_MAX_* and
     * REQUEST_SHUTDOWN per PRD §4.7 (applies globally since v1's only
     * modifier is global). */
    if (cfg->faults.stale_context_defaults.enabled) {
        const thermal_fault_detector_cfg_t *fc = &cfg->faults.stale_context_defaults;
        for (uint8_t c = 0; c < cfg->context_count; c++) {
            uint32_t ms_since = core->context_ever_valid[c]
                ? (uint32_t)(in->now_ms - core->context_last_valid_ms[c])
                : 0xFFFFFFFFu;
            uint8_t prev = core->stale_context_states[c].state;
            thermal_fault_stale_context_step(&core->stale_context_states[c], fc,
                                             ms_since,
                                             cfg->contexts[c].timeout_ms,
                                             in->now_ms);
            emit_fault_transition(core, THERMAL_FAULT_TYPE_STALE_CONTEXT,
                                  cfg->contexts[c].id, prev,
                                  core->stale_context_states[c].state,
                                  in->now_ms);
            if (fault_state_active(core->stale_context_states[c].state)) {
                int forces_max = action_forces_pwm_max(fc->action);
                int requests_sd = (fc->action == THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN);
                if (forces_max || requests_sd) {
                    /* v1: stale_context affects every actuator (the
                     * only modifier is global). */
                    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
                        slew_override_up[a] = 1;
                        force_pwm_max[a] = 1;
                        if (forces_max) {
                            cause_stale[a] = 1;
                        } else {
                            shutdown_action[a] = 1;
                        }
                    }
                    if (requests_sd && prev == THERMAL_FAULT_NORMAL
                        && core->cb.log_event) {
                        core->cb.log_event(in->now_ms, TEVENT_SHUTDOWN_REQUEST,
                                           (uint32_t)cfg->contexts[c].id,
                                           (uint32_t)THERMAL_FAULT_TYPE_STALE_CONTEXT,
                                           0u, 0u);
                    }
                }
            }
        }
    }

    /* Stuck-sensor force-max / request-shutdown action dispatch.
     * Detector was stepped in the pre-pass for USE_ZONE_FALLBACK; this
     * loop walks the same states again to apply force-max / request-
     * shutdown to all actuators of zones referencing the stuck sensor. */
    if (cfg->faults.stuck_sensor_defaults.enabled) {
        const thermal_fault_detector_cfg_t *fc = &cfg->faults.stuck_sensor_defaults;
        int forces_max = action_forces_pwm_max(fc->action);
        int requests_sd = (fc->action == THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN);
        if (forces_max || requests_sd) {
            for (uint8_t s = 0; s < cfg->sensor_count; s++) {
                if (!fault_state_active(core->stuck_sensor_states[s].state)) continue;
                /* Affected actuators: every actuator of every zone
                 * referencing this sensor. */
                for (uint8_t z = 0; z < cfg->zone_count; z++) {
                    const thermal_zone_cfg_t *zc = &cfg->zones[z];
                    int refs_sensor = 0;
                    for (uint8_t k = 0; k < zc->sensor_count; k++) {
                        if (zc->sensor_ids[k] == cfg->sensors[s].id) {
                            refs_sensor = 1;
                            break;
                        }
                    }
                    if (!refs_sensor) continue;
                    for (uint8_t k = 0; k < zc->actuator_count; k++) {
                        int slot = find_actuator_slot(cfg, zc->actuator_ids[k]);
                        if (slot < 0) continue;
                        slew_override_up[slot] = 1;
                        force_pwm_max[slot] = 1;
                        if (forces_max) {
                            cause_stuck[slot] = 1;
                        } else {
                            shutdown_action[slot] = 1;
                        }
                    }
                }
            }
        }
    }

    /* Edge-trigger shutdown event on any zone reaching SHUTDOWN severity.
     * Codex-B: also latch shutdown_request_latched so the state flag
     * persists across ticks per PRD §4.7 "latch a shutdown-request
     * condition." */
    for (uint8_t z = 0; z < cfg->zone_count; z++) {
        uint8_t curr_sev = zone_max_severity(&core->zones[z], &cfg->zones[z]);
        if (curr_sev >= THERMAL_TRIP_SHUTDOWN
            && core->prev_zone_max_severity[z] < THERMAL_TRIP_SHUTDOWN) {
            if (core->cb.log_event) {
                core->cb.log_event(in->now_ms, TEVENT_SHUTDOWN_REQUEST,
                                   (uint32_t)z, (uint32_t)curr_sev, 0u, 0u);
            }
        }
        if (curr_sev >= THERMAL_TRIP_SHUTDOWN) {
            core->shutdown_request_latched = 1;
        }
        core->prev_zone_max_severity[z] = curr_sev;
    }

    /* Codex-B: any actuator with an active shutdown_action[] this tick
     * (from a fault REQUEST_SHUTDOWN action) latches the
     * shutdown_request_latched flag. The actual event was emitted on the
     * detector's rising edge above. */
    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
        if (shutdown_action[a]) {
            core->shutdown_request_latched = 1;
        }
    }

    /* Apply force_pwm_max and emit safety-override events on rising edge.
     * CRITICAL trips alone trigger slew_override_up but NOT force_pwm_max
     * (PRD §4.7 -- critical trips request state_pwm[cooling_state], not
     * pwm_max). force_pwm_max is set only by SHUTDOWN severity or fault
     * actions. */
    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
        if (force_pwm_max[a]) {
            arb_pwm[a] = cfg->actuators[a].pwm_max;
        }
        if (slew_override_up[a] && !core->actuator_prev_safety_override[a]
            && core->cb.log_event) {
            core->cb.log_event(in->now_ms, TEVENT_SAFETY_OVERRIDE,
                               (uint32_t)cfg->actuators[a].id,
                               (uint32_t)actuator_max_zone_sev[a], 0u, 0u);
        }
    }

#if THERMALCORE_ENABLE_FAN_HEALTH
    /* Stage 17 (PRD Appendix C): advisory fan-health detector. Runs
     * after the fault detectors -- so it sees this tick's force-max /
     * shutdown overrides -- and before step 10, while actuator_prev_duty
     * and actuator_prev_slew_limited still hold the *previous* tick's
     * applied values: the drive that produced this tick's tach sample
     * (PRD C.2). Telemetry only; never touches arb_pwm. */
    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
        uint16_t fh_spinup_ticks = (cfg->control_period_ms > 0)
            ? (uint16_t)((cfg->actuators[a].spinup_ms +
                          cfg->control_period_ms - 1) /
                         cfg->control_period_ms)
            : 0;
        thermal_fan_health_step(&core->fan_health_states[a],
                                &cfg->fan_health[a],
                                core->actuator_prev_duty[a],
                                core->actuator_tach_rpm[a],
                                core->actuator_tach_valid[a],
                                core->actuator_prev_slew_limited[a],
                                (uint8_t)(force_pwm_max[a] ||
                                          shutdown_action[a]),
                                fh_spinup_ticks);
    }
#endif

    /* === §4.6 step 10: slew + final clamp + write output frame === */
    out->actuator_cmd_count = cfg->actuator_count;
    for (uint8_t a = 0; a < cfg->actuator_count; a++) {
        const thermal_actuator_cfg_t *ac = &cfg->actuators[a];
        uint8_t spinup_applied = 0;
        uint8_t dwell_applied = 0;
        uint8_t requested_pwm = arb_pwm[a];
        uint8_t safety_bypass_dwell = (uint8_t)(slew_override_up[a] ||
                                                force_pwm_max[a] ||
                                                shutdown_action[a]);

        if (!safety_bypass_dwell) {
            if (requested_pwm > 0 &&
                core->actuator_prev_duty[a] == 0 &&
                core->actuator_off_ticks[a] < ac->min_off_ticks) {
                requested_pwm = 0;
                dwell_applied = 1;
            } else if (requested_pwm == 0 &&
                       core->actuator_prev_duty[a] > 0 &&
                       core->actuator_on_ticks[a] < ac->min_on_ticks) {
                requested_pwm = core->actuator_prev_duty[a];
                dwell_applied = 1;
            }
        }

        if (requested_pwm == 0) {
            core->actuator_spinup_remaining[a] = 0;
        } else {
            uint16_t spinup_ticks = (cfg->control_period_ms > 0)
                ? (uint16_t)((ac->spinup_ms + cfg->control_period_ms - 1) /
                             cfg->control_period_ms)
                : 0;
            if (ac->spinup_ms > 0 &&
                spinup_ticks > 0 &&
                core->actuator_prev_duty[a] == 0 &&
                core->actuator_spinup_remaining[a] == 0) {
                core->actuator_spinup_remaining[a] = spinup_ticks;
            }
            if (core->actuator_spinup_remaining[a] > 0 &&
                ac->spinup_pwm > requested_pwm) {
                requested_pwm = ac->spinup_pwm;
                spinup_applied = 1;
            }
        }

        thermal_slew_result_t sr;
        thermal_slew_step(core->actuator_prev_duty[a],
                          requested_pwm,
                          ac->slew_per_tick,
                          (uint8_t)(slew_override_up[a] || spinup_applied),
                          &sr);

        /* Final clamp per PRD §4.3 line 506: 0 is the special off
         * command; non-zero requests below pwm_min are raised to pwm_min;
         * requests above pwm_max are lowered to pwm_max. When the
         * controller's post-fault request is 0 (no cooling demanded), the
         * slew is allowed to transition through values below pwm_min on
         * the way down -- otherwise the fan would stick at pwm_min
         * forever once commanded off. */
        uint8_t intended_off = (requested_pwm == 0);
        uint8_t final_pwm = sr.new_duty;
        if (final_pwm != 0 && !intended_off && final_pwm < ac->pwm_min) {
            final_pwm = ac->pwm_min;
        }
        if (final_pwm > ac->pwm_max) final_pwm = ac->pwm_max;

        /* Reason precedence (highest wins): shutdown > fault force-max
         * (stall/runaway) > modifier acoustic cap > governor (pid|step).
         * CRITICAL trip alone produces a GOVERNOR_* reason -- the cap
         * skip + slew bypass are observable via TEVENT_SAFETY_OVERRIDE,
         * not the reason code. */
        uint16_t reason = THERMAL_ACT_REASON_NONE;
        if (final_pwm > 0) {
            int has_pid_zone = 0;
            for (uint8_t z = 0; z < cfg->zone_count; z++) {
                for (uint8_t k = 0; k < cfg->zones[z].actuator_count; k++) {
                    if (cfg->zones[z].actuator_ids[k] == ac->id) {
                        if (cfg->zones[z].governor == THERMAL_GOVERNOR_PID) {
                            has_pid_zone = 1;
                        }
                        break;
                    }
                }
            }
            reason = has_pid_zone ? THERMAL_ACT_REASON_GOVERNOR_PID
                                  : THERMAL_ACT_REASON_GOVERNOR_STEP;
            if (modifier_cap_applied[a]) {
                reason = THERMAL_ACT_REASON_MODIFIER_ACOUSTIC_CAP;
            }
        }
        if (spinup_applied && final_pwm > 0) {
            reason = THERMAL_ACT_REASON_SPINUP;
        }
        if (dwell_applied) {
            reason = THERMAL_ACT_REASON_DWELL;
        }
        /* Fault force-max overrides governor/modifier reason. Codex-C
         * v3-#7: each detector kind gets its own reason code (instead
         * of stuck-sensor/stale-context borrowing FAULT_RUNAWAY). */
        if (cause_stall[a]) {
            reason = THERMAL_ACT_REASON_FAULT_STALL;
        }
        if (cause_stuck[a]) {
            reason = THERMAL_ACT_REASON_FAULT_STUCK_SENSOR;
        }
        if (cause_runaway[a]) {
            reason = THERMAL_ACT_REASON_FAULT_RUNAWAY;
        }
        if (cause_stale[a]) {
            reason = THERMAL_ACT_REASON_FAULT_STALE_CONTEXT;
        }
        /* Shutdown overrides everything: SHUTDOWN trip severity OR a
         * request_shutdown fault action. */
        if (actuator_max_zone_sev[a] >= THERMAL_TRIP_SHUTDOWN
            || shutdown_action[a]) {
            reason = THERMAL_ACT_REASON_SAFETY_SHUTDOWN;
        }

        out->actuator_cmds[a].actuator_id = ac->id;
        out->actuator_cmds[a].duty_0_255  = final_pwm;
        out->actuator_cmds[a].reason      = reason;

        core->actuator_prev_duty[a]            = final_pwm;
        core->actuator_prev_slew_limited[a]    = sr.slew_limited;
        core->actuator_prev_reason[a]          = reason;
        core->actuator_prev_safety_override[a] = slew_override_up[a];
        if (core->actuator_spinup_remaining[a] > 0) {
            core->actuator_spinup_remaining[a]--;
        }
        if (final_pwm > 0) {
            if (core->actuator_on_ticks[a] < 0xFFFF) {
                core->actuator_on_ticks[a]++;
            }
            core->actuator_off_ticks[a] = 0;
        } else {
            if (core->actuator_off_ticks[a] < 0xFFFF) {
                core->actuator_off_ticks[a]++;
            }
            core->actuator_on_ticks[a] = 0;
        }
    }

    /* === §4.6 step 11: telemetry emission on cadence === */
    if (cfg->telemetry.enable
        && cfg->telemetry.period_ticks > 0
        && (core->tick_count % cfg->telemetry.period_ticks) == 0
        && core->cb.telemetry_emit) {
        for (uint8_t i = 0; i < cfg->telemetry.enabled_signal_count; i++) {
            uint16_t sig = cfg->telemetry.enabled_signal_ids[i];
            int32_t value;
            if (dispatch_signal_value(core, sig, &value)) {
                core->cb.telemetry_emit(in->now_ms, sig, value);
            }
        }
    }
    core->tick_count++;
    core->last_now_ms = in->now_ms;

    return THERMAL_OK;
}

thermal_status_t thermal_core_apply_command(thermal_core_t *ctx,
                                            uint32_t now_ms,
                                            const thermal_command_t *cmd,
                                            thermal_command_result_t *result) {
    if (!ctx || !cmd || !result) return THERMAL_ERR_INVALID_ARG;
    thermal_core_internal_t *core = (thermal_core_internal_t *)ctx;
    if (core->cfg == NULL) return THERMAL_ERR_STATE;
    const thermal_config_t *cfg = core->cfg;

    thermal_status_t status;
    uint16_t target_id = 0;   /* event arg a2 */

    switch (cmd->command_id) {
    case THERMAL_CMD_SET_PID: {
        uint16_t z = cmd->u.set_pid.zone_id;
        target_id = z;
        if (z >= cfg->zone_count) {
            status = THERMAL_ERR_INVALID_ARG;
        } else if (cfg->zones[z].governor != THERMAL_GOVERNOR_PID) {
            status = THERMAL_ERR_INVALID_ARG;
        } else {
            const thermal_pid_cfg_t *b = &cfg->zones[z].pid;
            int32_t kp = cmd->u.set_pid.kp_q16;
            int32_t ki = cmd->u.set_pid.ki_q16;
            int32_t kd = cmd->u.set_pid.kd_q16;
            if (kp < b->kp_min_q16 || kp > b->kp_max_q16 ||
                ki < b->ki_min_q16 || ki > b->ki_max_q16 ||
                kd < b->kd_min_q16 || kd > b->kd_max_q16) {
                status = THERMAL_ERR_BOUNDS;
            } else {
                core->zones[z].shadow_pid.kp_q16 = kp;
                core->zones[z].shadow_pid.ki_q16 = ki;
                core->zones[z].shadow_pid.kd_q16 = kd;
                /* PRD §7.5: reset integrator + derivative history on
                 * gain change to avoid long unwind. */
                thermal_pid_reset(&core->zones[z].pid);
                status = THERMAL_OK;
            }
        }
        break;
    }
    case THERMAL_CMD_SET_SETPOINT: {
        uint16_t z = cmd->u.set_setpoint.zone_id;
        target_id = z;
        if (z >= cfg->zone_count) {
            status = THERMAL_ERR_INVALID_ARG;
        } else if (cfg->zones[z].governor != THERMAL_GOVERNOR_PID) {
            status = THERMAL_ERR_INVALID_ARG;
        } else {
            int32_t sp = cmd->u.set_setpoint.setpoint_mc;
            const thermal_pid_cfg_t *b = &cfg->zones[z].pid;
            if (sp < b->setpoint_min_mc || sp > b->setpoint_max_mc) {
                status = THERMAL_ERR_BOUNDS;
            } else if (!runtime_setpoint_with_modifier_offsets_valid(core, z, sp)) {
                status = THERMAL_ERR_BOUNDS;
            } else {
                core->zones[z].shadow_pid.setpoint_mc = sp;
                /* No PID reset: setpoint changes don't wind anti-windup
                 * (D is on measurement, not error). */
                status = THERMAL_OK;
            }
        }
        break;
    }
    case THERMAL_CMD_SET_TRIP: {
        uint16_t z = cmd->u.set_trip.zone_id;
        uint16_t t = cmd->u.set_trip.trip_idx;
        target_id = z;
        if (z >= cfg->zone_count) {
            status = THERMAL_ERR_INVALID_ARG;
        } else if (t >= cfg->zones[z].trip_count) {
            status = THERMAL_ERR_BOUNDS;
        } else {
            int32_t temp = cmd->u.set_trip.temp_mc;
            int32_t hyst = cmd->u.set_trip.hyst_mc;
            const thermal_trip_cfg_t *trips = core->zones[z].shadow_trips;
            uint8_t cnt = cfg->zones[z].trip_count;
            thermal_trip_cfg_t proposed[THERMAL_MAX_TRIPS_PER_ZONE];
            for (uint8_t i = 0; i < cnt; i++) {
                proposed[i] = trips[i];
            }
            proposed[t].temp_mc = temp;
            proposed[t].hyst_mc = hyst;
            int ok = (hyst >= 0);
            if (ok && t > 0) {
                if (temp <= trips[t - 1].temp_mc) ok = 0;
                if ((int64_t)temp - (int64_t)hyst <
                    (int64_t)trips[t - 1].temp_mc) ok = 0;
            }
            if (ok && (t + 1) < cnt) {
                if (temp >= trips[t + 1].temp_mc) ok = 0;
                int64_t next_cold = (int64_t)trips[t + 1].temp_mc -
                                    (int64_t)trips[t + 1].hyst_mc;
                if (next_cold < (int64_t)temp) ok = 0;
            }
            if (ok) {
                if (!zone_fallback_reaches_safety_floor(&cfg->zones[z], proposed)) {
                    ok = 0;
                }
            }
            if (ok && !runtime_trips_with_modifier_offsets_valid(core, proposed, cnt)) {
                ok = 0;
            }
            if (!ok) {
                status = THERMAL_ERR_BOUNDS;
            } else {
                core->zones[z].shadow_trips[t].temp_mc = temp;
                core->zones[z].shadow_trips[t].hyst_mc = hyst;
                status = THERMAL_OK;
            }
        }
        break;
    }
    case THERMAL_CMD_SET_CURVE_POINT: {
        uint16_t m = cmd->u.set_curve_point.modifier_id;
        uint16_t p = cmd->u.set_curve_point.point_idx;
        target_id = m;
        if (m >= cfg->modifier_count) {
            status = THERMAL_ERR_INVALID_ARG;
        } else if (p >= cfg->modifiers[m].curve_count) {
            status = THERMAL_ERR_BOUNDS;
        } else {
            int32_t x = cmd->u.set_curve_point.x;
            const thermal_curve_point_t *pts = core->shadow_modifiers[m].curve;
            uint8_t cnt = cfg->modifiers[m].curve_count;
            int ok = 1;
            if (p > 0 && x <= pts[p - 1].x) ok = 0;
            if ((p + 1) < cnt && x >= pts[p + 1].x) ok = 0;
            if (ok) {
                thermal_modifier_cfg_t proposed = core->shadow_modifiers[m];
                proposed.curve[p].x = x;
                proposed.curve[p].value0 = cmd->u.set_curve_point.value0;
                proposed.curve[p].value1 = cmd->u.set_curve_point.value1;
                if (!modifier_curve_values_valid_runtime(core, &proposed)) {
                    ok = 0;
                }
            }
            if (!ok) {
                status = THERMAL_ERR_BOUNDS;
            } else {
                core->shadow_modifiers[m].curve[p].x = x;
                core->shadow_modifiers[m].curve[p].value0 =
                    cmd->u.set_curve_point.value0;
                core->shadow_modifiers[m].curve[p].value1 =
                    cmd->u.set_curve_point.value1;
                status = THERMAL_OK;
            }
        }
        break;
    }
    case THERMAL_CMD_CLEAR_FAULT: {
        uint16_t ftype = cmd->u.clear_fault.fault_type;
        uint16_t tid = cmd->u.clear_fault.target_id;
        target_id = tid;
        switch (ftype) {
        case THERMAL_FAULT_TYPE_STALL: {
            int slot = find_actuator_slot(cfg, tid);
            if (slot < 0) {
                status = THERMAL_ERR_INVALID_ARG;
            } else if (core->stall_states[slot].state == THERMAL_FAULT_NORMAL) {
                status = THERMAL_OK;
            } else if (!thermal_fault_stall_clear_allowed(
                        &core->stall_states[slot],
                        &cfg->faults.stall_defaults)) {
                status = THERMAL_ERR_REJECTED_SAFETY;
            } else {
                thermal_fault_stall_reset(&core->stall_states[slot]);
                status = THERMAL_OK;
            }
            break;
        }
        case THERMAL_FAULT_TYPE_STUCK_SENSOR: {
            int slot = find_sensor_slot(cfg, tid);
            if (slot < 0) {
                status = THERMAL_ERR_INVALID_ARG;
            } else if (core->stuck_sensor_states[slot].state == THERMAL_FAULT_NORMAL) {
                status = THERMAL_OK;
            } else if (!thermal_fault_stuck_sensor_clear_allowed(
                        &core->stuck_sensor_states[slot],
                        &cfg->faults.stuck_sensor_defaults)) {
                status = THERMAL_ERR_REJECTED_SAFETY;
            } else {
                thermal_fault_stuck_sensor_reset(&core->stuck_sensor_states[slot]);
                status = THERMAL_OK;
            }
            break;
        }
        case THERMAL_FAULT_TYPE_RUNAWAY: {
            /* Per 7c emission convention, runaway target_id is zone INDEX. */
            if (tid >= cfg->zone_count) {
                status = THERMAL_ERR_INVALID_ARG;
            } else if (core->runaway_states[tid].state == THERMAL_FAULT_NORMAL) {
                status = THERMAL_OK;
            } else if (!thermal_fault_runaway_clear_allowed(
                        &core->runaway_states[tid],
                        &cfg->faults.runaway_defaults)) {
                status = THERMAL_ERR_REJECTED_SAFETY;
            } else {
                thermal_fault_runaway_reset(&core->runaway_states[tid]);
                core->runaway_latched_active_ticks[tid] = 0;
                core->runaway_escalated_shutdown[tid] = 0;
                status = THERMAL_OK;
            }
            break;
        }
        case THERMAL_FAULT_TYPE_STALE_CONTEXT: {
            int slot = find_context_slot(cfg, tid);
            if (slot < 0) {
                status = THERMAL_ERR_INVALID_ARG;
            } else if (core->stale_context_states[slot].state == THERMAL_FAULT_NORMAL) {
                status = THERMAL_OK;
            } else if (!thermal_fault_stale_context_clear_allowed(
                        &core->stale_context_states[slot],
                        &cfg->faults.stale_context_defaults)) {
                status = THERMAL_ERR_REJECTED_SAFETY;
            } else {
                thermal_fault_stale_context_reset(&core->stale_context_states[slot]);
                status = THERMAL_OK;
            }
            break;
        }
        default:
            status = THERMAL_ERR_INVALID_ARG;
            break;
        }
        break;
    }
    default:
        status = THERMAL_ERR_INVALID_ARG;
        target_id = 0;
        break;
    }

    result->status = status;
    result->detail_code = 0;

    if (core->cb.log_event) {
        uint16_t code = (status == THERMAL_OK)
            ? TEVENT_COMMAND_APPLIED : TEVENT_COMMAND_REJECTED;
        core->cb.log_event(now_ms, code,
                           (uint32_t)cmd->command_id,
                           (uint32_t)target_id,
                           (uint32_t)status, 0u);
    }
    return status;
}

thermal_status_t thermal_core_get_state(const thermal_core_t *ctx,
                                        thermal_state_snapshot_t *state) {
    if (!ctx || !state) return THERMAL_ERR_INVALID_ARG;
    const thermal_core_internal_t *core = (const thermal_core_internal_t *)ctx;
    if (core->cfg == NULL) return THERMAL_ERR_STATE;
    const thermal_config_t *cfg = core->cfg;

    memset(state, 0, sizeof(*state));
    state->now_ms         = core->last_now_ms;
    state->zone_count     = cfg->zone_count;
    state->actuator_count = cfg->actuator_count;
    state->context_count  = cfg->context_count;
    state->modifier_count = cfg->modifier_count;

    /* Zones */
    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        const zone_runtime_t *zr = &core->zones[i];
        const thermal_zone_cfg_t *zc = &cfg->zones[i];
        state->zones[i].temp_mc          = zr->temp_mc;
        state->zones[i].active_trip_mask = zr->active_trip_mask;
        state->zones[i].cooling_state    = zr->cooling_state;
        state->zones[i].aggregation_valid = zr->aggregation_valid;
        state->zones[i].effective_setpoint_mc =
            (zc->governor == THERMAL_GOVERNOR_PID)
                ? zr->effective_setpoint_mc
                : 0;
    }

    /* Actuators */
    for (uint8_t i = 0; i < cfg->actuator_count; i++) {
        state->actuators[i].requested_duty_0_255 = core->actuator_prev_requested[i];
        state->actuators[i].duty_0_255           = core->actuator_prev_duty[i];
        state->actuators[i].rpm                  = core->actuator_tach_rpm[i];
        state->actuators[i].tach_valid           = core->actuator_tach_valid[i];
        state->actuators[i].slew_limited         = core->actuator_prev_slew_limited[i];
        state->actuators[i].reason               = core->actuator_prev_reason[i];
    }

    /* Contexts */
    for (uint8_t i = 0; i < cfg->context_count; i++) {
        state->contexts[i].filtered_value = core->context_filters[i].filtered_value;
        state->contexts[i].valid          = core->context_filters[i].valid;
        state->contexts[i].ms_since_last_valid = core->context_ever_valid[i]
            ? (uint32_t)(core->last_now_ms - core->context_last_valid_ms[i])
            : 0xFFFFFFFFu;
    }

    /* Modifiers */
    for (uint8_t i = 0; i < cfg->modifier_count; i++) {
        state->modifiers[i].modifier_id     = i;
        state->modifiers[i].active          = core->modifier_active[i];
        int32_t cap = core->modifier_pwm_cap[i];
        if (cap < 0) cap = 0;
        if (cap > 255) cap = 255;
        state->modifiers[i].pwm_cap_0_255   = (uint8_t)cap;
        state->modifiers[i].trip_offset_mc  = core->modifier_trip_offset[i];
    }

    /* Faults: emit a snapshot entry for each active (non-NORMAL) detector. */
    uint8_t fc = 0;
    uint32_t flags = 0;
    for (uint8_t i = 0; i < cfg->actuator_count && fc < THERMAL_MAX_FAULTS; i++) {
        if (core->stall_states[i].state != THERMAL_FAULT_NORMAL) {
            state->faults[fc].fault_type    = THERMAL_FAULT_TYPE_STALL;
            state->faults[fc].target_id     = cfg->actuators[i].id;
            state->faults[fc].state         = core->stall_states[i].state;
            state->faults[fc].entered_ts_ms = core->stall_states[i].entered_ts_ms;
            fc++;
            flags |= THERMAL_STATE_ANY_FAULT_ACTIVE;
        }
    }
    for (uint8_t i = 0; i < cfg->sensor_count && fc < THERMAL_MAX_FAULTS; i++) {
        if (core->stuck_sensor_states[i].state != THERMAL_FAULT_NORMAL) {
            state->faults[fc].fault_type    = THERMAL_FAULT_TYPE_STUCK_SENSOR;
            state->faults[fc].target_id     = cfg->sensors[i].id;
            state->faults[fc].state         = core->stuck_sensor_states[i].state;
            state->faults[fc].entered_ts_ms = core->stuck_sensor_states[i].entered_ts_ms;
            fc++;
            flags |= THERMAL_STATE_ANY_FAULT_ACTIVE;
        }
    }
    for (uint8_t i = 0; i < cfg->zone_count && fc < THERMAL_MAX_FAULTS; i++) {
        if (core->runaway_states[i].state != THERMAL_FAULT_NORMAL) {
            state->faults[fc].fault_type    = THERMAL_FAULT_TYPE_RUNAWAY;
            state->faults[fc].target_id     = i;
            state->faults[fc].state         = core->runaway_states[i].state;
            state->faults[fc].entered_ts_ms = core->runaway_states[i].entered_ts_ms;
            fc++;
            flags |= THERMAL_STATE_ANY_FAULT_ACTIVE;
        }
    }
    for (uint8_t i = 0; i < cfg->context_count && fc < THERMAL_MAX_FAULTS; i++) {
        if (core->stale_context_states[i].state != THERMAL_FAULT_NORMAL) {
            state->faults[fc].fault_type    = THERMAL_FAULT_TYPE_STALE_CONTEXT;
            state->faults[fc].target_id     = cfg->contexts[i].id;
            state->faults[fc].state         = core->stale_context_states[i].state;
            state->faults[fc].entered_ts_ms = core->stale_context_states[i].entered_ts_ms;
            fc++;
            /* Codex-B #5: stale_context counts as a fault per PRD §4.7
             * (it is one of the four detector types); set both flags. */
            flags |= THERMAL_STATE_ANY_CONTEXT_STALE;
            flags |= THERMAL_STATE_ANY_FAULT_ACTIVE;
        }
    }
    state->fault_count = fc;

    /* Aggregate flags */
    for (uint8_t i = 0; i < cfg->actuator_count; i++) {
        if (core->actuator_prev_safety_override[i]) {
            flags |= THERMAL_STATE_ANY_SAFETY_OVERRIDE;
            break;
        }
    }
    /* Codex-B #4: SHUTDOWN_REQUESTED is driven by the persistent latch,
     * not a tick-edge zone-severity check (which fluctuates if temp
     * dips below SHUTDOWN). */
    if (core->shutdown_request_latched) {
        flags |= THERMAL_STATE_SHUTDOWN_REQUESTED;
    }
    state->flags = flags;

    return THERMAL_OK;
}
