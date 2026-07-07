/* core/thermal_core.h
 *
 * Top-level public API for thermal-core. Includes the config struct
 * hierarchy and the five v1 entry points. Pinned by PRD §4.3 (lines
 * 357-482) and the lifetime contract in PRD §4.3 lines 485-518.
 */
#ifndef THERMAL_CORE_H
#define THERMAL_CORE_H

#include <stdint.h>
#include "thermal_config.h"
#include "thermal_types.h"
#include "thermal_platform.h"
#include "thermal_commands.h"
#include "thermal_fan_health.h"   /* compile-gated; empty when off */

/* === Per-sensor config === */
typedef struct {
    uint16_t id;
    char     name[THERMAL_NAME_MAX];
    int32_t  iir_alpha_q16;
    uint32_t max_staleness_ms;
} thermal_sensor_cfg_t;

/* === Per-context-signal config === */
typedef struct {
    uint16_t id;
    char     name[THERMAL_NAME_MAX];
    uint8_t  unit;              /* thermal_context_unit_t */
    int32_t  iir_alpha_q16;
    uint32_t timeout_ms;
    uint8_t  fail_safe;         /* thermal_context_failsafe_t */
} thermal_context_cfg_t;

/* === Per-actuator config === */
typedef struct {
    uint16_t id;
    char     name[THERMAL_NAME_MAX];
    uint8_t  pwm_min;
    uint8_t  pwm_max;
    uint8_t  slew_per_tick;
    uint8_t  spinup_pwm;
    uint32_t spinup_ms;
    uint16_t min_on_ticks;      /* 0 disables anti-short-cycle on dwell */
    uint16_t min_off_ticks;     /* 0 disables anti-short-cycle off dwell */
    uint8_t  state_pwm[THERMAL_MAX_COOLING_STATES];
#if THERMALCORE_ENABLE_PID
    thermal_curve_point_t duty_linearization[THERMAL_MAX_CURVE_POINTS];
    uint8_t  duty_linearization_count;
#endif
} thermal_actuator_cfg_t;

/* === Per-zone PID config (gains + bounds + dt clamp) === */
typedef struct {
    int32_t  kp_q16, ki_q16, kd_q16;
#if THERMALCORE_ENABLE_PID
    int32_t  d_filter_alpha_q16;
#endif
    int32_t  setpoint_mc;
    int32_t  kp_min_q16, kp_max_q16;
    int32_t  ki_min_q16, ki_max_q16;
    int32_t  kd_min_q16, kd_max_q16;
    int32_t  setpoint_min_mc, setpoint_max_mc;
    uint16_t dt_min_ms, dt_max_ms;
} thermal_pid_cfg_t;

/* === Per-trip-point config === */
typedef struct {
    int32_t temp_mc;
    int32_t hyst_mc;
    uint8_t severity;           /* thermal_trip_severity_t */
    uint8_t cooling_state;
} thermal_trip_cfg_t;

/* === Per-zone config === */
typedef struct {
    char     name[THERMAL_NAME_MAX];
    uint16_t sensor_ids[THERMAL_MAX_SENSORS_PER_ZONE];
    int32_t  sensor_weights_q16[THERMAL_MAX_SENSORS_PER_ZONE];
    uint8_t  sensor_count;
    uint8_t  aggregation;       /* thermal_aggregation_t */
    int32_t  fallback_temp_mc;
    uint8_t  governor;          /* thermal_governor_t */
    thermal_pid_cfg_t pid;
    uint16_t actuator_ids[THERMAL_MAX_ACTUATORS_PER_ZONE];
    uint8_t  actuator_count;
    thermal_trip_cfg_t trips[THERMAL_MAX_TRIPS_PER_ZONE];
    uint8_t  trip_count;
} thermal_zone_cfg_t;

/* === Per-policy-modifier config === */
typedef struct {
    char     name[THERMAL_NAME_MAX];
    uint16_t context_id;
    uint8_t  stages;            /* bitmask of thermal_modifier_stage_t */
    thermal_curve_point_t curve[THERMAL_MAX_CURVE_POINTS];
    uint8_t  curve_count;
    uint8_t  fail_safe;         /* thermal_context_failsafe_t */
} thermal_modifier_cfg_t;

/* === Per-fault-detector config (generic, instanced by detector type) === */
typedef struct {
    uint8_t  enabled;
    uint8_t  severity;          /* thermal_fault_severity_t */
    uint8_t  action;            /* thermal_fault_action_t */
    uint16_t persist_ticks;
    uint16_t recovery_ticks;
    int32_t  threshold0;
    int32_t  threshold1;
    int32_t  threshold2;
    uint16_t correlated_context_id;
} thermal_fault_detector_cfg_t;

/* === All-detector defaults bundle === */
typedef struct {
    thermal_fault_detector_cfg_t stall_defaults;
    thermal_fault_detector_cfg_t stuck_sensor_defaults;
    thermal_fault_detector_cfg_t runaway_defaults;
    thermal_fault_detector_cfg_t stale_context_defaults;
} thermal_fault_detection_cfg_t;

/* === Telemetry config (enabled_signal_ids resolved by config loader) === */
typedef struct {
    uint8_t  enable;
    uint16_t period_ticks;
    uint16_t enabled_signal_ids[THERMAL_MAX_TELEMETRY_SIGNALS];
    uint8_t  enabled_signal_count;
} thermal_telemetry_cfg_t;

/* === Master config (immutable for the lifetime of a thermal_core_t) === */
typedef struct {
    uint16_t config_version;
    uint16_t control_period_ms;
    uint16_t period_relative_to_ms;  /* 0 = unspecified; else must match control_period_ms */
    thermal_sensor_cfg_t sensors[THERMAL_MAX_SENSORS];
    uint8_t  sensor_count;
    thermal_context_cfg_t contexts[THERMAL_MAX_CONTEXT_SIGNALS];
    uint8_t  context_count;
    thermal_actuator_cfg_t actuators[THERMAL_MAX_ACTUATORS];
    uint8_t  actuator_count;
    thermal_zone_cfg_t zones[THERMAL_MAX_ZONES];
    uint8_t  zone_count;
    thermal_modifier_cfg_t modifiers[THERMAL_MAX_MODIFIERS];
    uint8_t  modifier_count;
    thermal_fault_detection_cfg_t faults;
    thermal_telemetry_cfg_t telemetry;
#if THERMALCORE_ENABLE_FAN_HEALTH
    /* Stage 17 (PRD Appendix C): per-actuator fan-health detector
     * config, indexed by actuator slot. Compile-gated -- absent on
     * MCU builds, which keeps the v1 thermal_config_t layout intact. */
    thermal_fan_health_cfg_t fan_health[THERMAL_MAX_ACTUATORS];
#endif
} thermal_config_t;

/* === v1 public API (PRD §4.3 lines 470-482) ===
 *
 * Lifetime contract (PRD §4.3 lines 485-518):
 * - thermal_core_t is caller-owned, fixed-size, allocated by caller
 *   (static/global/stack). Core never allocates it.
 * - validate_config should be called before init so init failures are
 *   limited to invalid args or storage misuse.
 * - The thermal_config_t passed to init must remain alive and immutable
 *   for the context's lifetime.
 * - callbacks struct is copied by value during init; its function
 *   pointers must remain callable while the context is active.
 * - thermal_input_snapshot_t and its samples[] are borrowed only for the
 *   duration of thermal_core_step().
 * - A single thermal_core_t is not reentrant; serialize all API calls
 *   on it from one control thread.
 */
thermal_status_t thermal_core_validate_config(const thermal_config_t *cfg);

thermal_status_t thermal_core_init(thermal_core_t *ctx,
                                   const thermal_config_t *cfg,
                                   const thermal_core_callbacks_t *cb);

thermal_status_t thermal_core_step(thermal_core_t *ctx,
                                   const thermal_input_snapshot_t *in,
                                   thermal_output_frame_t *out);

thermal_status_t thermal_core_apply_command(thermal_core_t *ctx,
                                            uint32_t now_ms,
                                            const thermal_command_t *cmd,
                                            thermal_command_result_t *result);

thermal_status_t thermal_core_get_state(const thermal_core_t *ctx,
                                        thermal_state_snapshot_t *state);

#endif /* THERMAL_CORE_H */
