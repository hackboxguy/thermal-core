/* platform/esp32_idf/main/bsp_esp32_tach.h
 *
 * Stage 13 13a -- GPIO-ISR-based fan-tachometer edge counter.
 *
 * Wraps `gpio_config_t` + `gpio_isr_handler_add` to count
 * falling edges on the fan's tach line, with a 1 ms inter-edge
 * filter (empirically validated on the user's bench: Noctua
 * NF-A8 emits ~73 Hz max, leaving 13x margin against EMI bursts).
 *
 * `bsp_esp32_tach_read_ticks_delta()` returns the count delta
 * since the last call and atomically zeroes the counter, so
 * callers can sample on any cadence without losing ticks.
 *
 * Noctua NF-A8 conversion: RPM ~= ticks/s * 30 (2 pulses per
 * revolution).
 */
#ifndef BSP_ESP32_TACH_H
#define BSP_ESP32_TACH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the GPIO ISR + pull-up on `gpio_num`.  Typical:
 * GPIO 5 with an external 10 k pull-up to 3.3 V (per the user's
 * working board).  Returns 0 on success, -1 on IDF failure. */
int bsp_esp32_tach_init(int gpio_num);

/* Read the tick count since the last call to this function (or
 * since init for the first call).  Atomic exchange under the
 * hood so concurrent ISR firings don't lose edges. */
uint32_t bsp_esp32_tach_read_ticks_delta(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ESP32_TACH_H */
