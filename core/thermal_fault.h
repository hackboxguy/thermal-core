/* core/thermal_fault.h
 *
 * Fault detectors per PRD §4.7. Internal core header -- not included
 * from core/thermal_core.h. Four detectors, one struct + step + reset +
 * clear_allowed per type:
 *
 *   stall          -- per actuator
 *   stuck_sensor   -- per sensor
 *   runaway        -- per zone
 *   stale_context  -- per context signal
 *
 * Each detector runs a shared FSM (NORMAL -> DEGRADED/CRITICAL/LATCHED ->
 * RECOVERING -> NORMAL) per PRD §4.7 lines 621-627, with the entry/exit
 * conditions specific to the detector kind.
 *
 * action_to_state mapping (see thermal_fault.c):
 *   FORCE_PWM_MAX_AND_LATCH or REQUEST_SHUTDOWN -> LATCHED
 *   else severity CRITICAL -> CRITICAL
 *   else                   -> DEGRADED
 *
 * LATCHED never auto-recovers. recovery_count still increments when the
 * underlying condition is inactive; thermal_fault_*_clear_allowed checks
 * recovery_count >= cfg->recovery_ticks to decide whether Stage 8's
 * CMD_CLEAR_FAULT can transition LATCHED back to NORMAL.
 *
 * Stage 6 ships only the detector math + their unit tests. Integration
 * into thermal_core_step is deferred to Stage 7 (which gives stall the
 * commanded_pwm via arbitration, and stuck/stale the context tracking).
 */
#ifndef THERMAL_FAULT_H
#define THERMAL_FAULT_H

#include <stdint.h>
#include "thermal_config.h"
#include "thermal_types.h"
#include "thermal_core.h"     /* for thermal_fault_detector_cfg_t */

/* ============================================================
 * Stall detector  (per actuator)
 * ============================================================ */

typedef struct {
    uint8_t  state;                  /* thermal_fault_state_t */
    uint16_t persist_count;
    uint16_t recovery_count;
    uint32_t entered_ts_ms;
    uint16_t spinup_remaining;       /* ticks remaining in spin-up grace */
    uint8_t  prev_pwm_was_zero;      /* for spin-up entry detection */
} thermal_fault_stall_state_t;

void thermal_fault_stall_reset(thermal_fault_stall_state_t *s);

/* Cfg fields used:
 *   threshold0 = stall_rpm
 *   threshold1 = stall_pwm_threshold
 *   persist_ticks, recovery_ticks
 *   severity, action
 *   enabled, correlated_context_id (unused) */
void thermal_fault_stall_step(thermal_fault_stall_state_t *s,
                              const thermal_fault_detector_cfg_t *cfg,
                              uint8_t  requested_pwm,
                              uint16_t tach_rpm,
                              uint8_t  tach_valid,
                              uint8_t  spinup_pwm,
                              uint16_t spinup_ticks,
                              uint32_t now_ms);

int thermal_fault_stall_clear_allowed(const thermal_fault_stall_state_t *s,
                                      const thermal_fault_detector_cfg_t *cfg);

/* ============================================================
 * Stuck-sensor detector  (per sensor)
 * ============================================================ */

typedef struct {
    uint8_t  state;
    uint16_t persist_count;
    uint16_t recovery_count;
    uint32_t entered_ts_ms;
    int32_t  window_value_min;
    int32_t  window_value_max;
    uint16_t window_tick_count;      /* ticks accumulated in current window */
    uint8_t  window_correlated_seen_change;
} thermal_fault_stuck_sensor_state_t;

void thermal_fault_stuck_sensor_reset(thermal_fault_stuck_sensor_state_t *s);

/* Cfg fields used:
 *   threshold0 = delta_mc
 *   threshold1 = window_ticks
 *   persist_ticks, recovery_ticks
 *   severity, action
 *   correlated_context_id  (0xFFFF -> flatness-only mode)
 *
 * `correlated_load_changing` is a caller-supplied boolean; the caller
 * (Stage 7 wiring) computes it from a configured context signal's
 * value delta. The detector accumulates it across the same sensor
 * flatness window. */
void thermal_fault_stuck_sensor_step(thermal_fault_stuck_sensor_state_t *s,
                                     const thermal_fault_detector_cfg_t *cfg,
                                     int32_t  sensor_filtered_value,
                                     uint8_t  sensor_valid,
                                     uint8_t  correlated_load_changing,
                                     uint32_t now_ms);

int thermal_fault_stuck_sensor_clear_allowed(
    const thermal_fault_stuck_sensor_state_t *s,
    const thermal_fault_detector_cfg_t *cfg);

/* ============================================================
 * Runaway detector  (per zone)
 * ============================================================ */

typedef struct {
    uint8_t  state;
    uint16_t persist_count;          /* unused (window IS the persist) */
    uint16_t recovery_count;
    uint32_t entered_ts_ms;
    int32_t  temp_window[THERMAL_FAULT_RUNAWAY_WINDOW_MAX];
    uint8_t  pwm_window [THERMAL_FAULT_RUNAWAY_WINDOW_MAX];
    uint16_t window_head;
    uint8_t  window_filled;
} thermal_fault_runaway_state_t;

void thermal_fault_runaway_reset(thermal_fault_runaway_state_t *s);

/* Cfg fields used:
 *   threshold0    = rise_mc_threshold
 *   threshold1    = cooling_pwm_threshold
 *   persist_ticks  (caller must ensure <= THERMAL_FAULT_RUNAWAY_WINDOW_MAX)
 *   recovery_ticks
 *   severity, action */
void thermal_fault_runaway_step(thermal_fault_runaway_state_t *s,
                                const thermal_fault_detector_cfg_t *cfg,
                                int32_t  zone_temp_mc,
                                uint8_t  commanded_pwm,
                                uint32_t now_ms);

int thermal_fault_runaway_clear_allowed(const thermal_fault_runaway_state_t *s,
                                        const thermal_fault_detector_cfg_t *cfg);

/* ============================================================
 * Stale-context detector  (per context signal)
 * ============================================================ */

typedef struct {
    uint8_t  state;
    uint16_t persist_count;
    uint16_t recovery_count;
    uint32_t entered_ts_ms;
} thermal_fault_stale_context_state_t;

void thermal_fault_stale_context_reset(thermal_fault_stale_context_state_t *s);

/* Cfg fields used:
 *   persist_ticks, recovery_ticks
 *   severity, action
 *
 * Caller passes the context's ms_since_last_valid and configured
 * timeout_ms (from thermal_context_cfg_t.timeout_ms). */
void thermal_fault_stale_context_step(
    thermal_fault_stale_context_state_t *s,
    const thermal_fault_detector_cfg_t *cfg,
    uint32_t ms_since_last_valid,
    uint32_t context_timeout_ms,
    uint32_t now_ms);

int thermal_fault_stale_context_clear_allowed(
    const thermal_fault_stale_context_state_t *s,
    const thermal_fault_detector_cfg_t *cfg);

#endif /* THERMAL_FAULT_H */
