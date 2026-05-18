/* platform/ch32v003/ch32_callbacks.h
 *
 * Stage 18e -- thermal_core_callbacks_t wiring for the CH32V003.
 * The CH32 counterpart of platform/esp32_idf/main/esp32_callbacks.h.
 *
 * THERMALCORE_CH32_TELEMETRY (compile flag, default 0):
 *   0 -- ch32_log_event_cb is a no-op; telemetry is not emitted.
 *   1 -- ch32_log_event_cb / ch32_telemetry_emit_cb format one
 *        canonical CSV row each (test/parity/canonical.c, the same
 *        serializer the host parity binary and the scenario runner
 *        use) and push it over USART1 via bsp_ch32_uart.
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

#if THERMALCORE_CH32_TELEMETRY
/* thermal_core_callbacks_t.telemetry_emit (telemetry build only) */
void ch32_telemetry_emit_cb(uint32_t ts_ms, uint16_t signal_id,
                            int32_t value);
#endif

#endif /* CH32_CALLBACKS_H */
