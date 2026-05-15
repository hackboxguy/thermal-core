/* platform/esp32_idf/main/bsp_esp32_tach.h
 *
 * Slot-indexed GPIO-ISR fan-tachometer edge counter.  Each
 * actuator slot owns one GPIO + one ISR handler + one per-slot
 * counter; counters are read+zeroed atomically by
 * `bsp_esp32_tach_read_ticks_delta(slot)`.
 *
 * Empirical 1 ms inter-edge filter rejects EMI bursts.
 * Noctua NF-A8 max edge rate at 2200 RPM is ~73 Hz -> 13x
 * margin against the filter.
 *
 * Noctua NF-A8 RPM conversion: RPM ~= ticks/s * 30
 * (2 pulses per revolution).
 */
#ifndef BSP_ESP32_TACH_H
#define BSP_ESP32_TACH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the GPIO ISR + pull-up on `gpio_num` for actuator
 * slot `slot`.  Each slot uses an independent counter.  The
 * IDF-level `gpio_install_isr_service` is idempotent across
 * calls -- the first slot's init installs it; subsequent calls
 * return INVALID_STATE, which we treat as success.
 *
 * Returns 0 on success, -1 if `slot >= THERMAL_MAX_ACTUATORS`
 * or an IDF call fails. */
int bsp_esp32_tach_init(uint8_t slot, int gpio_num);

/* Read the tick count for `slot` since the previous call (or
 * since init for the first call).  Atomic read-and-zero on the
 * C3 (single-issue 32-bit core; uint32_t reads are naturally
 * atomic).  Returns 0 if the slot was never initialised. */
uint32_t bsp_esp32_tach_read_ticks_delta(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ESP32_TACH_H */
