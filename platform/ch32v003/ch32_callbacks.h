/* platform/ch32v003/ch32_callbacks.h
 *
 * Stage 18e -- thermal_core_callbacks_t wiring for the CH32V003.
 * The CH32 counterpart of platform/esp32_idf/main/esp32_callbacks.h.
 *
 * THERMALCORE_CH32_TELEMETRY (compile flag, default 0):
 *   0 -- ch32_log_event_cb is a no-op; telemetry is not emitted.
 *   1 -- ch32_log_event_cb / ch32_telemetry_emit_cb only *enqueue*
 *        a compact record (they run inside thermal_core_step() and
 *        must return promptly -- PRD callback contract).
 *        ch32_telemetry_drain(), called from the main loop after
 *        the core step, formats the queued records as canonical CSV
 *        (test/parity/canonical.c) and pushes them over USART1, so
 *        the blocking UART writes stay out of the core's work path.
 *        Ring overflow increments a cumulative drop counter, reported
 *        on the next drain as TSIG_PLATFORM_CH32_TELEMETRY_DROPS.
 */
#ifndef CH32_CALLBACKS_H
#define CH32_CALLBACKS_H

#include <stdint.h>

#ifndef THERMALCORE_CH32_TELEMETRY
#define THERMALCORE_CH32_TELEMETRY 0
#endif

/* thermal_core_callbacks_t.log_event */
void ch32_log_event_cb(uint32_t ts_ms, uint16_t code,
                       uint32_t a1, uint32_t a2,
                       uint32_t a3, uint32_t a4);

/* Cumulative count of records dropped by the nonblocking telemetry ring.
 * Returns 0 in headless builds where the telemetry path is compiled out. */
uint32_t ch32_telemetry_drop_count(void);

#if THERMALCORE_CH32_TELEMETRY
/* thermal_core_callbacks_t.telemetry_emit (telemetry build only) */
void ch32_telemetry_emit_cb(uint32_t ts_ms, uint16_t signal_id,
                            int32_t value);

/* Drain queued telemetry records to USART1 as canonical CSV.
 * Call once per tick from the main loop, after thermal_core_step()
 * has returned -- never from inside a callback. */
void ch32_telemetry_drain(void);
#endif

#endif /* CH32_CALLBACKS_H */
