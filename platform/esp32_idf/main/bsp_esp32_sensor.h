/* platform/esp32_idf/main/bsp_esp32_sensor.h
 *
 * Multi-device DS18B20 wrapper.  One shared 1-Wire bus drives
 * up to `THERMAL_MAX_SENSORS` DS18B20 devices in parallel
 * (multi-drop bus; each DS18B20 has a factory-assigned 64-bit
 * ROM address that distinguishes it on the bus).
 *
 * Slot mapping: the espressif/ds18b20 driver enumerates ROMs
 * in deterministic ROM-address order, so slot N corresponds to
 * the Nth-smallest ROM on the bus.  This mapping is stable
 * across reboots; bench labels can be correlated against the
 * ROM addresses logged at init.
 *
 * Typical wiring: GPIO 6 with one 4.7 kOhm pull-up to 3.3 V
 * shared across all DS18B20s; each DS18B20 VDD on 3.3 V
 * (parasite power disabled).
 *
 * v1 limitations:
 *   - One bus only (all sensors share `onewire_gpio`).
 *   - All sensors at 12-bit resolution.
 *   - Single shared 750 ms conversion (skip-rom convert), then
 *     per-slot read by ROM address.
 */
#ifndef BSP_ESP32_SENSOR_H
#define BSP_ESP32_SENSOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the 1-Wire bus on `gpio_num`, enumerate exactly
 * `expected_count` DS18B20 devices, set 12-bit resolution on
 * each.  Logs each device's ROM address at INFO level.
 *
 * Returns 0 on success, -1 if `expected_count == 0`,
 * `expected_count > THERMAL_MAX_SENSORS`, the bus fails to
 * open, or the wrong number of devices is discovered (helps
 * the user notice missing pull-up or missing sensor before the
 * daemon silently fallback-temps). */
int bsp_esp32_sensor_init(int gpio_num, uint8_t expected_count);

/* Trigger a conversion (12-bit, ~750 ms shared across all
 * sensors via skip-rom convert), wait, read back the device
 * at `slot`, convert to millicelsius.
 *
 * Returns 0 on success (and writes `*out_mc`); -1 on conversion
 * or read failure or out-of-range slot (and leaves `*out_mc`
 * unchanged). */
int bsp_esp32_sensor_read_mc(uint8_t slot, int32_t *out_mc);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ESP32_SENSOR_H */
