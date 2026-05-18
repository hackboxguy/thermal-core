/* platform/ch32v003/ch32_callbacks.c
 *
 * Stage 18e -- thermal_core_callbacks_t wiring for the CH32V003.
 * See ch32_callbacks.h. Mirrors the REPLAY branch of
 * platform/esp32_idf/main/esp32_callbacks.c: format one canonical
 * CSV row per callback through the shared serializer, then emit it.
 */
#include "ch32_callbacks.h"

#if THERMALCORE_CH32_TELEMETRY

#include "canonical.h"
#include "bsp_ch32_uart.h"

void ch32_log_event_cb(uint32_t ts_ms, uint16_t code,
                       uint32_t a1, uint32_t a2,
                       uint32_t a3, uint32_t a4)
{
    char buf[128];
    int  n = thermalcore_canonical_event(buf, sizeof buf,
                                         ts_ms, code, a1, a2, a3, a4);
    if (n > 0) {
        bsp_ch32_uart_puts(buf);
    }
}

void ch32_telemetry_emit_cb(uint32_t ts_ms, uint16_t signal_id,
                            int32_t value)
{
    char buf[128];
    int  n = thermalcore_canonical_sample(buf, sizeof buf,
                                          ts_ms, signal_id, value, 0);
    if (n > 0) {
        bsp_ch32_uart_puts(buf);
    }
}

#else  /* telemetry build disabled: headless STANDALONE */

void ch32_log_event_cb(uint32_t ts_ms, uint16_t code,
                       uint32_t a1, uint32_t a2,
                       uint32_t a3, uint32_t a4)
{
    (void)ts_ms; (void)code;
    (void)a1; (void)a2; (void)a3; (void)a4;
}

#endif /* THERMALCORE_CH32_TELEMETRY */
