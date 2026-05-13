/* platform/esp32_idf/main/esp32_callbacks.h
 *
 * Stage 13 13b -- thin printf wrappers for the core's
 * thermal_core_callbacks_t.  Non-blocking + alloc-free per
 * PRD section 4.4.
 *
 * Side state: `esp32_callbacks_last_zone_temp_mc(zone)` returns
 * the most recent TSIG_ZONE_TEMP_<zone> value the core emitted
 * this tick, and `_last_actuator_pwm(slot)` returns the most
 * recent TSIG_ACTUATOR_DUTY_<slot>.  app_main consults both
 * once per second to render its human-readable status line.
 */
#ifndef ESP32_CALLBACKS_H
#define ESP32_CALLBACKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp32_log_event_cb(uint32_t ts_ms, uint16_t code,
                        uint32_t a1, uint32_t a2,
                        uint32_t a3, uint32_t a4);

void esp32_telemetry_emit_cb(uint32_t ts_ms, uint16_t signal_id,
                              int32_t value);

/* Cached values for app_main's status line.  Returns INT32_MIN
 * if the core has not yet emitted that signal. */
int32_t esp32_callbacks_last_zone_temp_mc(uint8_t zone);
int32_t esp32_callbacks_last_actuator_pwm(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* ESP32_CALLBACKS_H */
