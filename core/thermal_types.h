/* core/thermal_types.h
 *
 * Public enums, snapshot/output/state-inspection structs, the curve-point
 * struct, and the caller-owned thermal_core_t context.
 *
 * Sourced verbatim from PRD §4.3 (lines 146-355). Numeric values are part
 * of the v1 wire/log contract — do not renumber existing members; new ones
 * append.
 */
#ifndef THERMAL_TYPES_H
#define THERMAL_TYPES_H

#include <stdint.h>
#include "thermal_config.h"

/* === Status === */
typedef enum {
    THERMAL_OK                  = 0x0000,
    THERMAL_ERR_INVALID_ARG     = 0x0001,
    THERMAL_ERR_INVALID_CONFIG  = 0x0002,
    THERMAL_ERR_BOUNDS          = 0x0003,
    THERMAL_ERR_STATE           = 0x0004,
    THERMAL_ERR_UNAVAILABLE     = 0x0005,
    THERMAL_ERR_NO_SPACE        = 0x0006,
    THERMAL_ERR_REJECTED_SAFETY = 0x0007
} thermal_status_t;

/* === Sample kind (discriminator for thermal_sample_t.kind) === */
typedef enum {
    THERMAL_SAMPLE_TEMP_MC,
    THERMAL_SAMPLE_TACH_RPM,
    THERMAL_SAMPLE_CONTEXT_I32
} thermal_sample_kind_t;

/* === Governor selector === */
typedef enum {
    THERMAL_GOVERNOR_STEP_WISE = 0x01,
    THERMAL_GOVERNOR_PID       = 0x02
} thermal_governor_t;

/* === Sensor aggregation === */
typedef enum {
    THERMAL_AGG_MAX      = 0x01,
    THERMAL_AGG_AVG      = 0x02,
    THERMAL_AGG_WEIGHTED = 0x03
} thermal_aggregation_t;

/* === Trip severity === */
typedef enum {
    THERMAL_TRIP_WARN     = 0x01,
    THERMAL_TRIP_CRITICAL = 0x02,
    THERMAL_TRIP_SHUTDOWN = 0x03
} thermal_trip_severity_t;

/* === Context signal unit === */
typedef enum {
    THERMAL_CONTEXT_UNIT_NONE       = 0x00,
    THERMAL_CONTEXT_UNIT_KMH        = 0x01,
    THERMAL_CONTEXT_UNIT_BOOL       = 0x02,
    THERMAL_CONTEXT_UNIT_RPM        = 0x03,
    THERMAL_CONTEXT_UNIT_CELSIUS_MC = 0x04
} thermal_context_unit_t;

/* === Context fail-safe === */
typedef enum {
    THERMAL_FAILSAFE_ASSUME_STATIONARY = 0x01,
    THERMAL_FAILSAFE_HOLD_LAST         = 0x02,
    THERMAL_FAILSAFE_ASSUME_VALUE      = 0x03
} thermal_context_failsafe_t;

/* === Policy modifier stages === */
typedef enum {
    THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET = 0x01,
    THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP    = 0x02
} thermal_modifier_stage_t;

/* === Fault severity === */
typedef enum {
    THERMAL_FAULT_SEVERITY_DEGRADED = 0x01,
    THERMAL_FAULT_SEVERITY_CRITICAL = 0x02
} thermal_fault_severity_t;

/* === Fault action === */
typedef enum {
    THERMAL_FAULT_ACTION_NONE                          = 0x00,
    THERMAL_FAULT_ACTION_MARK_DEGRADED                 = 0x01,
    THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK             = 0x02,
    THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED = 0x03,
    THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH       = 0x04,
    THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN              = 0x05
} thermal_fault_action_t;

/* === Fault state machine === */
typedef enum {
    THERMAL_FAULT_NORMAL,
    THERMAL_FAULT_DEGRADED,
    THERMAL_FAULT_CRITICAL,
    THERMAL_FAULT_LATCHED,
    THERMAL_FAULT_RECOVERING
} thermal_fault_state_t;

/* === Fault detector type === */
typedef enum {
    THERMAL_FAULT_TYPE_STALL         = 0x01,
    THERMAL_FAULT_TYPE_STUCK_SENSOR  = 0x02,
    THERMAL_FAULT_TYPE_RUNAWAY       = 0x03,
    THERMAL_FAULT_TYPE_STALE_CONTEXT = 0x04
} thermal_fault_type_t;

/* === Global state flags (bitmask) === */
typedef enum {
    THERMAL_STATE_ANY_FAULT_ACTIVE     = 0x00000001u,
    THERMAL_STATE_SHUTDOWN_REQUESTED   = 0x00000002u,
    THERMAL_STATE_ANY_CONTEXT_STALE    = 0x00000004u,
    THERMAL_STATE_ANY_SAFETY_OVERRIDE  = 0x00000008u
} thermal_state_flags_t;

/* === Actuator command origin reason === */
typedef enum {
    THERMAL_ACT_REASON_NONE                  = 0x0000,
    THERMAL_ACT_REASON_GOVERNOR_PID          = 0x0101,
    THERMAL_ACT_REASON_GOVERNOR_STEP         = 0x0102,
    THERMAL_ACT_REASON_MODIFIER_ACOUSTIC_CAP = 0x0201,
    THERMAL_ACT_REASON_FAULT_STALL           = 0x0301,
    THERMAL_ACT_REASON_FAULT_RUNAWAY         = 0x0302,
    THERMAL_ACT_REASON_FAULT_STUCK_SENSOR    = 0x0303,
    THERMAL_ACT_REASON_FAULT_STALE_CONTEXT   = 0x0304,
    THERMAL_ACT_REASON_SAFETY_SHUTDOWN       = 0x0401,
    THERMAL_ACT_REASON_MANUAL_CMD            = 0x0501,
    THERMAL_ACT_REASON_SPINUP                = 0x0601
} thermal_actuator_reason_t;

/* === Sample (platform → core via input snapshot) === */
typedef struct {
    uint16_t id;                /* sensor, actuator tach, or context signal ID */
    uint8_t  kind;              /* thermal_sample_kind_t */
    uint8_t  valid;             /* 0 = absent/stale/faulted */
    int32_t  value;             /* units determined by kind/config */
    uint32_t sample_ts_ms;
    uint16_t quality;           /* platform-defined; 0 = unknown/normal */
} thermal_sample_t;

typedef struct {
    uint32_t now_ms;
    const thermal_sample_t *samples;
    uint8_t sample_count;
} thermal_input_snapshot_t;

/* === Actuator output (core → platform via output frame) === */
typedef struct {
    uint16_t actuator_id;
    uint8_t  duty_0_255;
    uint16_t reason;            /* thermal_actuator_reason_t */
} thermal_actuator_cmd_t;

typedef struct {
    thermal_actuator_cmd_t actuator_cmds[THERMAL_MAX_ACTUATORS];
    uint8_t actuator_cmd_count;
} thermal_output_frame_t;

/* === Per-zone runtime state (state inspection) === */
typedef struct {
    int32_t  temp_mc;
    uint32_t active_trip_mask;
    uint8_t  cooling_state;     /* step-wise state, or PID safety-floor state */
    uint8_t  aggregation_valid; /* 1 if temp_mc came from >= 1 valid sensor */
    int32_t  effective_setpoint_mc;  /* 0 for step-wise zones in v1 */
} thermal_zone_state_t;

/* === Per-actuator runtime state === */
typedef struct {
    uint8_t  requested_duty_0_255;
    uint8_t  duty_0_255;
    uint16_t rpm;
    uint8_t  tach_valid;
    uint8_t  slew_limited;
    uint16_t reason;            /* thermal_actuator_reason_t */
} thermal_actuator_state_t;

/* === Per-fault-detector state snapshot ===
 *
 * target_id namespace per PRD §4.3:
 *   STALL         → actuator id
 *   STUCK_SENSOR  → sensor id
 *   RUNAWAY       → zone slot index in cfg->zones[] (v1: zones have
 *                   no separate numeric id field; the slot IS the
 *                   addressable identity, matching CMD_CLEAR_FAULT's
 *                   target_id convention)
 *   STALE_CONTEXT → context signal id
 */
typedef struct {
    uint8_t  fault_type;        /* thermal_fault_type_t */
    uint16_t target_id;
    uint8_t  state;             /* thermal_fault_state_t */
    uint32_t entered_ts_ms;     /* timestamp when current state was entered */
} thermal_fault_state_snapshot_t;

/* === Per-context-signal filter state === */
typedef struct {
    int32_t  filtered_value;
    uint8_t  valid;
    uint32_t ms_since_last_valid;
} thermal_context_state_t;

/* === Per-modifier runtime state === */
typedef struct {
    uint8_t modifier_id;
    uint8_t active;             /* 1 when modifier affects output this tick */
    uint8_t pwm_cap_0_255;
    int32_t trip_offset_mc;
} thermal_modifier_state_t;

/* === Full state snapshot (state inspection) === */
typedef struct {
    uint32_t now_ms;
    thermal_zone_state_t zones[THERMAL_MAX_ZONES];
    uint8_t  zone_count;
    thermal_actuator_state_t actuators[THERMAL_MAX_ACTUATORS];
    uint8_t  actuator_count;
    thermal_fault_state_snapshot_t faults[THERMAL_MAX_FAULTS];
    uint8_t  fault_count;
    thermal_context_state_t contexts[THERMAL_MAX_CONTEXT_SIGNALS];
    uint8_t  context_count;
    thermal_modifier_state_t modifiers[THERMAL_MAX_MODIFIERS];
    uint8_t  modifier_count;
    uint32_t flags;             /* thermal_state_flags_t bits */
} thermal_state_snapshot_t;

/* === Interpolation point (used by curves and modifiers) === */
typedef struct {
    int32_t x;
    int32_t value0;
    int32_t value1;
} thermal_curve_point_t;

/* === thermal_core_t — caller-owned context (PRD §4.3 line 487) ===
 *
 * Public, fixed-size, declared here so callers can allocate it on the
 * stack, in BSS, or globally. The real internal layout (sensor IIR state,
 * PID integrators, fault state machines, etc.) lives in core/thermal_core.c
 * and is private. A C99 compile-time fit check there asserts that the
 * internal struct fits in this reserved buffer.
 *
 * sizeof(thermal_core_t) depends on THERMAL_CORE_T_RESERVED_BYTES; v1
 * promises a stable source API, not a stable binary ABI across builds
 * with different maxima.
 */
typedef struct {
    unsigned char _reserved[THERMAL_CORE_T_RESERVED_BYTES];
} thermal_core_t;

#endif /* THERMAL_TYPES_H */
