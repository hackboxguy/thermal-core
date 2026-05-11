/* core/thermal_platform.h
 *
 * Platform callback surface. Sourced from PRD §4.4 (lines 523-531).
 *
 * Callbacks are invoked from thermal_core_step() and thermal_core_apply_command();
 * they must return promptly. Platforms must buffer, queue, coalesce, or drop
 * telemetry/log records rather than blocking on I/O, syslog, NVS, disk, or
 * cross-thread locks. Numeric-code → text rendering belongs in platform/tooling.
 *
 * During thermal_core_step(), all callback ts_ms values equal the input
 * snapshot's now_ms, so every callback from one step shares the same
 * deterministic timestamp. During thermal_core_apply_command(), ts_ms is
 * the now_ms parameter passed by the caller.
 */
#ifndef THERMAL_PLATFORM_H
#define THERMAL_PLATFORM_H

#include <stdint.h>

typedef struct {
    /* Discrete state-transition log: numeric event codes, up to four
     * uint32 args. Code namespace lives in core/thermal_events.h. */
    void (*log_event)(uint32_t ts_ms, uint16_t code, uint32_t a1,
                      uint32_t a2, uint32_t a3, uint32_t a4);

    /* Continuous numeric telemetry (zone temps, PWM/RPM, PID terms,
     * context values, policy outputs). Signal ID namespace lives in
     * core/thermal_signals.h. May be NULL to disable telemetry emit. */
    void (*telemetry_emit)(uint32_t ts_ms, uint16_t signal_id,
                           int32_t value);
} thermal_core_callbacks_t;

#endif /* THERMAL_PLATFORM_H */
