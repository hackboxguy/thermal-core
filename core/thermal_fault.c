/* core/thermal_fault.c -- fault detectors per PRD §4.7. */
#include "thermal_fault.h"

/* === Shared state-machine helper === */

static uint8_t action_to_state(uint8_t action, uint8_t severity) {
    if (action == THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH ||
        action == THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN) {
        return THERMAL_FAULT_LATCHED;
    }
    if (severity == THERMAL_FAULT_SEVERITY_CRITICAL) {
        return THERMAL_FAULT_CRITICAL;
    }
    return THERMAL_FAULT_DEGRADED;
}

static void fsm_tick(uint8_t  *state,
                     uint16_t *persist_count,
                     uint16_t *recovery_count,
                     uint32_t *entered_ts_ms,
                     uint8_t   condition_active,
                     uint8_t   cfg_severity,
                     uint8_t   cfg_action,
                     uint16_t  cfg_persist_ticks,
                     uint16_t  cfg_recovery_ticks,
                     uint32_t  now_ms) {
    switch (*state) {
    case THERMAL_FAULT_NORMAL:
        if (condition_active) {
            (*persist_count)++;
            if (*persist_count >= cfg_persist_ticks) {
                *state = action_to_state(cfg_action, cfg_severity);
                *entered_ts_ms = now_ms;
                *recovery_count = 0;
            }
        } else {
            *persist_count = 0;
        }
        break;

    case THERMAL_FAULT_DEGRADED:
    case THERMAL_FAULT_CRITICAL:
        if (condition_active) {
            *recovery_count = 0;
        } else {
            *state = THERMAL_FAULT_RECOVERING;
            *recovery_count = 0;
            *entered_ts_ms = now_ms;
        }
        break;

    case THERMAL_FAULT_RECOVERING:
        if (condition_active) {
            *state = action_to_state(cfg_action, cfg_severity);
            *persist_count = 0;
            *recovery_count = 0;
            *entered_ts_ms = now_ms;
        } else {
            (*recovery_count)++;
            if (*recovery_count >= cfg_recovery_ticks) {
                *state = THERMAL_FAULT_NORMAL;
                *persist_count = 0;
                *recovery_count = 0;
                *entered_ts_ms = now_ms;
            }
        }
        break;

    case THERMAL_FAULT_LATCHED:
        /* No auto-recovery; track inactive ticks so clear_allowed can
         * gate the explicit Stage-8 reset on recovery_ticks elapsed. */
        if (condition_active) {
            *recovery_count = 0;
        } else if (*recovery_count < 0xFFFF) {
            (*recovery_count)++;
        }
        break;

    default:
        /* Unknown state: defensive reset. */
        *state = THERMAL_FAULT_NORMAL;
        *persist_count = 0;
        *recovery_count = 0;
        break;
    }
}

/* ============================================================
 * Stall detector
 * ============================================================ */

void thermal_fault_stall_reset(thermal_fault_stall_state_t *s) {
    s->state = THERMAL_FAULT_NORMAL;
    s->persist_count = 0;
    s->recovery_count = 0;
    s->entered_ts_ms = 0;
    s->spinup_remaining = 0;
    s->prev_pwm_was_zero = 1;
}

void thermal_fault_stall_step(thermal_fault_stall_state_t *s,
                              const thermal_fault_detector_cfg_t *cfg,
                              uint8_t  requested_pwm,
                              uint16_t tach_rpm,
                              uint8_t  tach_valid,
                              uint8_t  spinup_pwm,
                              uint16_t spinup_ticks,
                              uint32_t now_ms) {
    (void)spinup_pwm;        /* reserved for future spin-up logic */

    if (!cfg->enabled) return;

    /* Spin-up grace: 0 -> non-zero PWM transition arms a countdown. */
    uint8_t pwm_is_zero = (requested_pwm == 0);
    if (s->prev_pwm_was_zero && !pwm_is_zero) {
        s->spinup_remaining = spinup_ticks;
    }
    s->prev_pwm_was_zero = pwm_is_zero;

    uint8_t in_spinup = (s->spinup_remaining > 0);
    if (in_spinup) {
        s->spinup_remaining--;
    }

    /* Condition: not in spinup grace, tach reading valid, requested PWM
     * above stall_pwm_threshold, and tach reading below stall_rpm. */
    uint8_t condition_active = 0;
    if (!in_spinup &&
        tach_valid &&
        requested_pwm >= (uint8_t)cfg->threshold1 &&
        tach_rpm < (uint16_t)cfg->threshold0) {
        condition_active = 1;
    }

    fsm_tick(&s->state, &s->persist_count, &s->recovery_count,
             &s->entered_ts_ms, condition_active,
             cfg->severity, cfg->action,
             cfg->persist_ticks, cfg->recovery_ticks, now_ms);
}

int thermal_fault_stall_clear_allowed(const thermal_fault_stall_state_t *s,
                                      const thermal_fault_detector_cfg_t *cfg) {
    return s->state == THERMAL_FAULT_LATCHED &&
           s->recovery_count >= cfg->recovery_ticks;
}

/* ============================================================
 * Stuck-sensor detector
 * ============================================================ */

void thermal_fault_stuck_sensor_reset(thermal_fault_stuck_sensor_state_t *s) {
    s->state = THERMAL_FAULT_NORMAL;
    s->persist_count = 0;
    s->recovery_count = 0;
    s->entered_ts_ms = 0;
    s->window_value_min = 0;
    s->window_value_max = 0;
    s->window_tick_count = 0;
    s->window_correlated_seen_change = 0;
}

void thermal_fault_stuck_sensor_step(thermal_fault_stuck_sensor_state_t *s,
                                     const thermal_fault_detector_cfg_t *cfg,
                                     int32_t  sensor_filtered_value,
                                     uint8_t  sensor_valid,
                                     uint8_t  correlated_load_changing,
                                     uint32_t now_ms) {
    if (!cfg->enabled) return;

    uint16_t window_ticks = (uint16_t)cfg->threshold1;
    int32_t  delta_threshold = cfg->threshold0;

    /* Window accumulation (valid samples only). */
    if (sensor_valid) {
        if (s->window_tick_count == 0) {
            s->window_value_min = sensor_filtered_value;
            s->window_value_max = sensor_filtered_value;
            s->window_correlated_seen_change = 0;
        } else {
            if (sensor_filtered_value < s->window_value_min)
                s->window_value_min = sensor_filtered_value;
            if (sensor_filtered_value > s->window_value_max)
                s->window_value_max = sensor_filtered_value;
        }
        if (correlated_load_changing) {
            s->window_correlated_seen_change = 1;
        }
        s->window_tick_count++;
    }

    uint8_t condition_active = 0;

    /* Window check fires once per window_ticks. Window_ticks == 0 is a
     * config bug; defensive: skip the check. */
    if (window_ticks > 0 && s->window_tick_count >= window_ticks) {
        uint8_t flatness_only_mode = (cfg->correlated_context_id == 0xFFFF);

        if (flatness_only_mode || s->window_correlated_seen_change) {
            int32_t delta = s->window_value_max - s->window_value_min;
            if (delta < delta_threshold) {
                condition_active = 1;
            }
        }

        /* Reset window. */
        s->window_value_min = sensor_filtered_value;
        s->window_value_max = sensor_filtered_value;
        s->window_tick_count = sensor_valid ? 1 : 0;
        s->window_correlated_seen_change = 0;
        if (sensor_valid && correlated_load_changing) {
            s->window_correlated_seen_change = 1;
        }
    }

    fsm_tick(&s->state, &s->persist_count, &s->recovery_count,
             &s->entered_ts_ms, condition_active,
             cfg->severity, cfg->action,
             cfg->persist_ticks, cfg->recovery_ticks, now_ms);
}

int thermal_fault_stuck_sensor_clear_allowed(
    const thermal_fault_stuck_sensor_state_t *s,
    const thermal_fault_detector_cfg_t *cfg) {
    return s->state == THERMAL_FAULT_LATCHED &&
           s->recovery_count >= cfg->recovery_ticks;
}

/* ============================================================
 * Runaway detector
 * ============================================================ */

void thermal_fault_runaway_reset(thermal_fault_runaway_state_t *s) {
    s->state = THERMAL_FAULT_NORMAL;
    s->persist_count = 0;
    s->recovery_count = 0;
    s->entered_ts_ms = 0;
    for (uint16_t i = 0; i < THERMAL_FAULT_RUNAWAY_WINDOW_MAX; i++) {
        s->temp_window[i] = 0;
        s->pwm_window[i] = 0;
    }
    s->window_head = 0;
    s->window_filled = 0;
}

void thermal_fault_runaway_step(thermal_fault_runaway_state_t *s,
                                const thermal_fault_detector_cfg_t *cfg,
                                int32_t  zone_temp_mc,
                                uint8_t  commanded_pwm,
                                uint32_t now_ms) {
    if (!cfg->enabled) return;

    uint16_t persist = cfg->persist_ticks;
    if (persist == 0) return;
    if (persist > THERMAL_FAULT_RUNAWAY_WINDOW_MAX) {
        persist = THERMAL_FAULT_RUNAWAY_WINDOW_MAX;
    }

    /* Push (temp, pwm) into the ring. */
    s->temp_window[s->window_head] = zone_temp_mc;
    s->pwm_window[s->window_head] = commanded_pwm;
    s->window_head = (uint16_t)((s->window_head + 1) % persist);
    if (s->window_head == 0) s->window_filled = 1;

    /* Condition check requires a full window of samples. */
    uint8_t condition_active = 0;
    if (s->window_filled) {
        /* N = newest sample slot (one back from head), M = oldest (head). */
        uint16_t newest = (uint16_t)((s->window_head + persist - 1) % persist);
        uint16_t oldest = s->window_head;
        int32_t temp_N = s->temp_window[newest];
        int32_t temp_M = s->temp_window[oldest];
        uint8_t pwm_min = 255;
        for (uint16_t i = 0; i < persist; i++) {
            if (s->pwm_window[i] < pwm_min) pwm_min = s->pwm_window[i];
        }
        if ((temp_N - temp_M) >= cfg->threshold0 &&
            pwm_min >= (uint8_t)cfg->threshold1) {
            condition_active = 1;
        }
    }

    /* Runaway's "persist period" is encoded in the window itself; pass
     * persist_ticks = 1 to the FSM so it transitions immediately on
     * a window-full condition. */
    fsm_tick(&s->state, &s->persist_count, &s->recovery_count,
             &s->entered_ts_ms, condition_active,
             cfg->severity, cfg->action,
             1, cfg->recovery_ticks, now_ms);
}

int thermal_fault_runaway_clear_allowed(const thermal_fault_runaway_state_t *s,
                                        const thermal_fault_detector_cfg_t *cfg) {
    return s->state == THERMAL_FAULT_LATCHED &&
           s->recovery_count >= cfg->recovery_ticks;
}

/* ============================================================
 * Stale-context detector
 * ============================================================ */

void thermal_fault_stale_context_reset(thermal_fault_stale_context_state_t *s) {
    s->state = THERMAL_FAULT_NORMAL;
    s->persist_count = 0;
    s->recovery_count = 0;
    s->entered_ts_ms = 0;
}

void thermal_fault_stale_context_step(
    thermal_fault_stale_context_state_t *s,
    const thermal_fault_detector_cfg_t *cfg,
    uint32_t ms_since_last_valid,
    uint32_t context_timeout_ms,
    uint32_t now_ms) {
    if (!cfg->enabled) return;

    uint8_t condition_active = (ms_since_last_valid >= context_timeout_ms) ? 1u : 0u;

    fsm_tick(&s->state, &s->persist_count, &s->recovery_count,
             &s->entered_ts_ms, condition_active,
             cfg->severity, cfg->action,
             cfg->persist_ticks, cfg->recovery_ticks, now_ms);
}

int thermal_fault_stale_context_clear_allowed(
    const thermal_fault_stale_context_state_t *s,
    const thermal_fault_detector_cfg_t *cfg) {
    return s->state == THERMAL_FAULT_LATCHED &&
           s->recovery_count >= cfg->recovery_ticks;
}
