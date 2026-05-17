/* platform/ch32v003/bsp_ch32_sensor.h
 *
 * Stage 18 -- CH32V003 DS18B20 temperature-sensor BSP wrapper.
 * Mirrors the ESP32 BSP shape (platform/esp32_idf/main/
 * bsp_esp32_sensor.h): one shared bit-banged 1-Wire bus, slot-indexed
 * reads. The tiny build profile caps the CH32V003 at one sensor.
 */
#ifndef BSP_CH32_SENSOR_H
#define BSP_CH32_SENSOR_H

#include <stdint.h>

/* Configure the 1-Wire bus on `pin` (ch32fun pin id) for
 * `expected_count` DS18B20 devices. Returns 0 on success. */
int bsp_ch32_sensor_init(int pin, uint8_t expected_count);

/* Read sensor `slot` in milli-degrees C into *out_mc. Returns 0 on
 * success, non-zero on a bus or CRC failure. */
int bsp_ch32_sensor_read_mc(uint8_t slot, int32_t *out_mc);

#endif /* BSP_CH32_SENSOR_H */
