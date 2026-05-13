/* platform/esp32_idf/main/bsp_esp32_sensor.h
 *
 * Stage 13 13a -- DS18B20 1-Wire temperature sensor wrapper.
 *
 * Uses the `espressif/onewire_bus` managed component (RMT-based
 * 1-Wire master) and `espressif/ds18b20` driver from the IDF
 * component registry.  Configures the first device found on the
 * bus for 12-bit resolution.  Read returns millicelsius int32
 * so the BSP boundary stays float-free -- the float conversion
 * lives entirely inside `bsp_esp32_sensor.c`.
 *
 * Typical wiring: GPIO 6 with a 4.7 kOhm pull-up to 3.3 V, DS18B20
 * VDD on 3.3 V (parasite power disabled).  Matches the user's
 * working board.
 */
#ifndef BSP_ESP32_SENSOR_H
#define BSP_ESP32_SENSOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the 1-Wire bus on `gpio_num`, enumerate the first
 * DS18B20, set 12-bit resolution.  Returns 0 on success, -1 on
 * bus/device open failure (e.g. missing pull-up, no sensor on
 * bus). */
int bsp_esp32_sensor_init(int gpio_num);

/* Trigger a conversion (12-bit takes <= 750 ms), wait, read
 * back, convert to millicelsius.  Caller pays the 800 ms
 * conversion wait inside this function.
 *
 * Returns 0 on success (and writes `*out_mc`); -1 on conversion
 * or read failure (and leaves `*out_mc` unchanged). */
int bsp_esp32_sensor_read_mc(int32_t *out_mc);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ESP32_SENSOR_H */
