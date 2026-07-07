/* core/thermal_signals.h
 *
 * Telemetry signal IDs emitted by the core via thermal_core_callbacks_t.
 * Pinned by PRD §7.1 (lines 856-882). IDs are part of the wire contract
 * (TELEM_SAMPLE frames) — the PRD locks the broad type ranges and the
 * PID-term formula; within-range sub-signal layout is core's choice and
 * defined here.
 *
 * Range layout (PRD §7.1 lines 873-882):
 *   0x0100..0x01FF  Zone temperatures, cooling state, zone-level outputs
 *                   (indexed by zone slot)
 *   0x0200..0x02FF  Actuator PWM/RPM/state (indexed by actuator slot)
 *   0x0300..0x03FF  PID terms: 0x0300 + zone*4 + term, where
 *                   term = 0 error, 1 integral, 2 derivative, 3 output
 *   0x0400..0x04FF  Context values (indexed by context slot)
 *   0x0500..0x05FF  Modifier outputs (indexed by modifier slot + sub)
 *   0x0600..0x06FF  Fault detector states (indexed by per-detector-kind
 *                   slot + sub)
 *
 * Within-range sub-signal layout (v1, 16 slots per sub-signal type):
 *   ZONE      0x00 temp_mc        0x10 cooling_state
 *             0x20 trip_mask      0x30 effective_setpoint_mc
 *             0x40 aggregation_valid
 *   ACTUATOR  0x00 duty           0x10 rpm
 *             0x20 slew_limited
 *   CONTEXT   0x00 filtered_value 0x10 valid
 *   MODIFIER  0x00 pwm_cap        0x10 trip_offset_mc
 *             0x20 active
 *   FAULT     0x00 stall state    0x10 stuck_sensor state
 *             0x20 runaway state  0x30 stale_context state
 *
 * Loaders expand wildcard selectors (e.g., `zone_temp_*`) once at config
 * load into thermal_telemetry_cfg_t.enabled_signal_ids[]. The core only
 * emits signals in that list, and validate_config rejects any signal id
 * that does not decode to a (range, sub, slot) supported by the current
 * configuration.
 */
#ifndef THERMAL_SIGNALS_H
#define THERMAL_SIGNALS_H

#include <stdint.h>

/* Typed alias for telemetry signal IDs. Numeric values come from the
 * macros below; the type is here so callers and tests can declare
 * variables/fields of the right shape. */
typedef uint16_t thermal_telemetry_signal_t;

/* === Range bases (PRD-locked) === */
#define TSIG_ZONE_BASE       0x0100u
#define TSIG_ACTUATOR_BASE   0x0200u
#define TSIG_PID_BASE        0x0300u
#define TSIG_CONTEXT_BASE    0x0400u
#define TSIG_MODIFIER_BASE   0x0500u
#define TSIG_FAULT_BASE      0x0600u
#define TSIG_HIL_BASE        0x0700u
#define TSIG_FAN_HEALTH_BASE 0x0800u

/* === Sub-signal strides within each range === */
#define TSIG_RANGE_SIZE      0x0100u
#define TSIG_SUB_STRIDE      0x0010u

/* === Zone sub-signals === */
#define TSIG_ZONE_SUB_TEMP                0x00u
#define TSIG_ZONE_SUB_COOLING_STATE       0x10u
#define TSIG_ZONE_SUB_TRIP_MASK           0x20u
#define TSIG_ZONE_SUB_EFFECTIVE_SETPOINT  0x30u
#define TSIG_ZONE_SUB_AGGREGATION_VALID   0x40u

#define TSIG_ZONE_TEMP(slot) \
    ((uint16_t)(TSIG_ZONE_BASE + TSIG_ZONE_SUB_TEMP + (slot)))
#define TSIG_ZONE_COOLING_STATE(slot) \
    ((uint16_t)(TSIG_ZONE_BASE + TSIG_ZONE_SUB_COOLING_STATE + (slot)))
#define TSIG_ZONE_TRIP_MASK(slot) \
    ((uint16_t)(TSIG_ZONE_BASE + TSIG_ZONE_SUB_TRIP_MASK + (slot)))
#define TSIG_ZONE_EFFECTIVE_SETPOINT(slot) \
    ((uint16_t)(TSIG_ZONE_BASE + TSIG_ZONE_SUB_EFFECTIVE_SETPOINT + (slot)))
#define TSIG_ZONE_AGGREGATION_VALID(slot) \
    ((uint16_t)(TSIG_ZONE_BASE + TSIG_ZONE_SUB_AGGREGATION_VALID + (slot)))

/* === Actuator sub-signals === */
#define TSIG_ACTUATOR_SUB_DUTY            0x00u
#define TSIG_ACTUATOR_SUB_RPM             0x10u
#define TSIG_ACTUATOR_SUB_SLEW_LIMITED    0x20u

#define TSIG_ACTUATOR_DUTY(slot) \
    ((uint16_t)(TSIG_ACTUATOR_BASE + TSIG_ACTUATOR_SUB_DUTY + (slot)))
#define TSIG_ACTUATOR_RPM(slot) \
    ((uint16_t)(TSIG_ACTUATOR_BASE + TSIG_ACTUATOR_SUB_RPM + (slot)))
#define TSIG_ACTUATOR_SLEW_LIMITED(slot) \
    ((uint16_t)(TSIG_ACTUATOR_BASE + TSIG_ACTUATOR_SUB_SLEW_LIMITED + (slot)))

/* === PID terms (PRD-locked formula) === */
#define TSIG_PID(zone, term) \
    ((uint16_t)(TSIG_PID_BASE + (zone) * 4u + (term)))
#define TSIG_PID_ERROR(zone)       TSIG_PID(zone, 0)
#define TSIG_PID_INTEGRAL(zone)    TSIG_PID(zone, 1)
#define TSIG_PID_DERIVATIVE(zone)  TSIG_PID(zone, 2)
#define TSIG_PID_OUTPUT(zone)      TSIG_PID(zone, 3)

/* === Context sub-signals === */
#define TSIG_CONTEXT_SUB_VALUE     0x00u
#define TSIG_CONTEXT_SUB_VALID     0x10u

#define TSIG_CONTEXT_VALUE(slot) \
    ((uint16_t)(TSIG_CONTEXT_BASE + TSIG_CONTEXT_SUB_VALUE + (slot)))
#define TSIG_CONTEXT_VALID(slot) \
    ((uint16_t)(TSIG_CONTEXT_BASE + TSIG_CONTEXT_SUB_VALID + (slot)))

/* === Modifier sub-signals === */
#define TSIG_MODIFIER_SUB_PWM_CAP        0x00u
#define TSIG_MODIFIER_SUB_TRIP_OFFSET    0x10u
#define TSIG_MODIFIER_SUB_ACTIVE         0x20u

#define TSIG_MODIFIER_PWM_CAP_AT(slot) \
    ((uint16_t)(TSIG_MODIFIER_BASE + TSIG_MODIFIER_SUB_PWM_CAP + (slot)))
#define TSIG_MODIFIER_TRIP_OFFSET(slot) \
    ((uint16_t)(TSIG_MODIFIER_BASE + TSIG_MODIFIER_SUB_TRIP_OFFSET + (slot)))
#define TSIG_MODIFIER_ACTIVE(slot) \
    ((uint16_t)(TSIG_MODIFIER_BASE + TSIG_MODIFIER_SUB_ACTIVE + (slot)))

/* === Fault detector state sub-signals ===
 * Each sub is indexed by its detector kind's natural slot space:
 *   STALL         per actuator slot
 *   STUCK_SENSOR  per sensor slot
 *   RUNAWAY       per zone slot
 *   STALE_CONTEXT per context slot
 */
#define TSIG_FAULT_SUB_STALL_STATE          0x00u
#define TSIG_FAULT_SUB_STUCK_SENSOR_STATE   0x10u
#define TSIG_FAULT_SUB_RUNAWAY_STATE        0x20u
#define TSIG_FAULT_SUB_STALE_CONTEXT_STATE  0x30u

#define TSIG_FAULT_STALL_STATE(actuator_slot) \
    ((uint16_t)(TSIG_FAULT_BASE + TSIG_FAULT_SUB_STALL_STATE + (actuator_slot)))
#define TSIG_FAULT_STUCK_SENSOR_STATE(sensor_slot) \
    ((uint16_t)(TSIG_FAULT_BASE + TSIG_FAULT_SUB_STUCK_SENSOR_STATE + (sensor_slot)))
#define TSIG_FAULT_RUNAWAY_STATE(zone_slot) \
    ((uint16_t)(TSIG_FAULT_BASE + TSIG_FAULT_SUB_RUNAWAY_STATE + (zone_slot)))
#define TSIG_FAULT_STALE_CONTEXT_STATE(context_slot) \
    ((uint16_t)(TSIG_FAULT_BASE + TSIG_FAULT_SUB_STALE_CONTEXT_STATE + (context_slot)))

/* === HIL ingress sub-signals (Stage 14) ===
 * Used only when the firmware is built in HIL_PERIPHERAL mode
 * (PRD §8.3): the ESP32 owns the sensors, emits TELEM_SAMPLE
 * frames over USB-CDC with these IDs, and the Linux daemon
 * ingests them into the next thermal_input_snapshot_t.
 *
 * SENSOR_TEMP carries the DS18B20 (or equivalent) temperature
 * in millidegrees Celsius -- daemon maps slot N to its
 * cfg->sensors[N] entry.
 * TACH_RPM carries the fan tach reading in RPM -- daemon maps
 * slot N to its cfg->actuators[N] entry.
 *
 * Promoted from provisional file-local #defines in Stage 14a/b's
 * platform/esp32_idf/main/main.c.  Slot-0 numeric values changed
 * (0x0701/0x0702 -> 0x0700/0x0710) when promoted; both firmware
 * and daemon must speak the canonical IDs.
 */
#define TSIG_HIL_SUB_SENSOR_TEMP     0x00u
#define TSIG_HIL_SUB_TACH_RPM        0x10u

#define TSIG_HIL_SENSOR_TEMP(slot) \
    ((uint16_t)(TSIG_HIL_BASE + TSIG_HIL_SUB_SENSOR_TEMP + (slot)))
#define TSIG_HIL_TACH_RPM(slot) \
    ((uint16_t)(TSIG_HIL_BASE + TSIG_HIL_SUB_TACH_RPM + (slot)))

/* === Fan-health detector sub-signals (Stage 17, PRD Appendix C) ===
 * Advisory-only fan-degradation telemetry, indexed per actuator slot.
 * The detector never commands an actuator; these are read-only health
 * indicators. 0x0700 is owned by HIL, so the range opens at 0x0800.
 *
 *   DELTA            health_delta_pct -- the negative-drift score,
 *                    <= 0 (a point above baseline contributes zero)
 *   SEVERITY         HEALTHY / AGING / DEGRADED / FAILING (0..3)
 *   BASELINE_SOURCE  field / factory / model / none (0..3) -- so a
 *                    consumer knows whether it is reading unit-aging
 *                    or model-deviation. `none` is telemetry-only,
 *                    reported for a disabled fan-health slot; it is
 *                    never a config-authorable baseline source.
 *   CONFIDENCE       count of baseline points with observed samples --
 *                    coverage, not sample recency
 */
#define TSIG_FAN_HEALTH_SUB_DELTA            0x00u
#define TSIG_FAN_HEALTH_SUB_SEVERITY         0x10u
#define TSIG_FAN_HEALTH_SUB_BASELINE_SOURCE  0x20u
#define TSIG_FAN_HEALTH_SUB_CONFIDENCE       0x30u

#define TSIG_FAN_HEALTH_DELTA(actuator_slot) \
    ((uint16_t)(TSIG_FAN_HEALTH_BASE + TSIG_FAN_HEALTH_SUB_DELTA + (actuator_slot)))
#define TSIG_FAN_HEALTH_SEVERITY(actuator_slot) \
    ((uint16_t)(TSIG_FAN_HEALTH_BASE + TSIG_FAN_HEALTH_SUB_SEVERITY + (actuator_slot)))
#define TSIG_FAN_HEALTH_BASELINE_SOURCE(actuator_slot) \
    ((uint16_t)(TSIG_FAN_HEALTH_BASE + TSIG_FAN_HEALTH_SUB_BASELINE_SOURCE + (actuator_slot)))
#define TSIG_FAN_HEALTH_CONFIDENCE(actuator_slot) \
    ((uint16_t)(TSIG_FAN_HEALTH_BASE + TSIG_FAN_HEALTH_SUB_CONFIDENCE + (actuator_slot)))

/* === Backward-compatibility aliases for PRD §7.1 example named
 * constants. Defined in terms of slot 0/1 of their respective ranges;
 * the underlying numeric IDs match the PRD examples. */
#define TSIG_ZONE_TEMP_SOC          TSIG_ZONE_TEMP(0)         /* 0x0100 */
#define TSIG_ZONE_TEMP_AMP          TSIG_ZONE_TEMP(1)         /* 0x0101 */
#define TSIG_ACTUATOR_PWM_MAIN_FAN  TSIG_ACTUATOR_DUTY(0)     /* 0x0200 */
#define TSIG_PID_ERROR_SOC          TSIG_PID_ERROR(0)         /* 0x0300 */
#define TSIG_PID_INTEGRAL_SOC       TSIG_PID_INTEGRAL(0)      /* 0x0301 */
#define TSIG_PID_DERIVATIVE_SOC     TSIG_PID_DERIVATIVE(0)    /* 0x0302 */
#define TSIG_SPEED_KMH              TSIG_CONTEXT_VALUE(0)     /* 0x0400 */

/* Note: the PRD §7.1 example also listed TSIG_ACTUATOR_RPM_MAIN_FAN
 * (0x0201), TSIG_MODIFIER_PWM_CAP (0x0401), and TSIG_FAULT_STALL_COUNT
 * (0x0600). Those names conflict with the v1 within-range layout
 * defined here; use the macros above instead:
 *   TSIG_ACTUATOR_RPM(0)               (numerically 0x0210)
 *   TSIG_MODIFIER_PWM_CAP_AT(0)        (numerically 0x0500)
 *   TSIG_FAULT_STALL_STATE(0)          (numerically 0x0600)
 * No wire codec exists yet (Stage 10), so the renumber is safe. */

#endif /* THERMAL_SIGNALS_H */
