/* core/thermal_events.h
 *
 * Discrete event codes emitted by the core via thermal_core_callbacks_t
 * log_event(). Pinned by PRD §7.1 (lines 886-896). Codes are part of the
 * wire contract (TELEM_EVENT frames) — do not renumber existing entries.
 *
 * Separate namespace from thermal_telemetry_signal_t (PRD §4.4 line 534).
 */
#ifndef THERMAL_EVENTS_H
#define THERMAL_EVENTS_H

#include <stdint.h>

typedef enum {
    /* Fault lifecycle (0x10xx) */
    TEVENT_FAULT_ENTER          = 0x1000,
    TEVENT_FAULT_RECOVERING     = 0x1001,
    TEVENT_FAULT_CLEAR          = 0x1002,

    /* Safety overrides and shutdown requests (0x11xx) */
    TEVENT_SAFETY_OVERRIDE      = 0x1100,
    TEVENT_SHUTDOWN_REQUEST     = 0x1101,

    /* Command application (0x12xx) */
    TEVENT_COMMAND_APPLIED      = 0x1200,
    TEVENT_COMMAND_REJECTED     = 0x1201,

    /* Zone aggregation fallback lifecycle (0x13xx) */
    TEVENT_ZONE_FALLBACK_ENTER  = 0x1300,
    TEVENT_ZONE_FALLBACK_EXIT   = 0x1301
} thermal_event_code_t;

#endif /* THERMAL_EVENTS_H */
