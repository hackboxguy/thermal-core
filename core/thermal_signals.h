/* core/thermal_signals.h
 *
 * Telemetry signal IDs emitted by the core via thermal_core_callbacks_t.
 * Pinned by PRD §7.1 (lines 856-882). IDs are part of the wire contract
 * (TELEM_SAMPLE frames) — do not renumber existing entries.
 *
 * Range conventions per PRD §7.1:
 *   0x0100..0x01FF  Zone temperatures, cooling state, zone outputs
 *                   (indexed by zone slot)
 *   0x0200..0x02FF  Actuator PWM/RPM/state (indexed by actuator slot)
 *   0x0300..0x03FF  PID terms: 0x0300 + zone*4 + term, where
 *                   term = 0 error, 1 integral, 2 derivative, 3 output
 *   0x0400..0x04FF  Context values (indexed by context slot)
 *   0x0500..0x05FF  Modifier outputs (indexed by modifier slot + offset)
 *   0x0600..0x06FF  Fault detector counters/states (indexed by slot)
 *
 * Loaders expand wildcard selectors (e.g., `zone_temp_*`) once at config
 * load into thermal_telemetry_cfg_t.enabled_signal_ids[]. The core only
 * emits signals in that list.
 */
#ifndef THERMAL_SIGNALS_H
#define THERMAL_SIGNALS_H

#include <stdint.h>

typedef enum {
    /* Zone temperatures and outputs (0x0100..0x01FF) */
    TSIG_ZONE_TEMP_SOC          = 0x0100,
    TSIG_ZONE_TEMP_AMP          = 0x0101,

    /* Actuator PWM/RPM (0x0200..0x02FF) */
    TSIG_ACTUATOR_PWM_MAIN_FAN  = 0x0200,
    TSIG_ACTUATOR_RPM_MAIN_FAN  = 0x0201,

    /* PID terms (0x0300..0x03FF) */
    TSIG_PID_ERROR_SOC          = 0x0300,
    TSIG_PID_INTEGRAL_SOC       = 0x0301,
    TSIG_PID_DERIVATIVE_SOC     = 0x0302,

    /* Context values (0x0400..0x04FF) */
    TSIG_SPEED_KMH              = 0x0400,
    TSIG_MODIFIER_PWM_CAP       = 0x0401,

    /* Fault counters/states (0x0600..0x06FF) */
    TSIG_FAULT_STALL_COUNT      = 0x0600
} thermal_telemetry_signal_t;

#endif /* THERMAL_SIGNALS_H */
