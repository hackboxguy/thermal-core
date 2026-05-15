/* platform/esp32_idf/main/esp32_pinmap.h
 *
 * Stage 14e -- JSON-driven pin map for the ESP32 firmware.
 *
 * Decouples GPIO assignments from main.c so that adding a second
 * fan or sensor is a JSON-only change (edit `configs/
 * esp32-c3-standalone.json`, rebuild with `make build-esp32`,
 * flash).  json2static.py emits a `const esp32_pinmap_t
 * G_ESP32_PINMAP` from the JSON's `esp32_pinmap` section,
 * alongside the existing `G_THERMAL_CFG`.
 *
 * v1 design constraints:
 *   - One 1-Wire bus shared by all DS18B20 sensors (the canonical
 *     PRD §5.1 example).  json2static rejects configs that declare
 *     different `onewire_gpio` values for different sensor slots.
 *   - Each fan owns its own LEDC channel + tach GPIO.  Up to
 *     THERMAL_MAX_ACTUATORS (=2) fans.
 *   - All fans share LEDC_TIMER_0 -> same PWM frequency for every
 *     fan.  Mixed-frequency would need per-fan timer allocation;
 *     deferred.
 *
 * Slot order in this struct matches slot order in
 * thermal_config_t.sensors / actuators -- json2static.py resolves
 * by name so the JSON-side order is free.
 */
#ifndef ESP32_PINMAP_H
#define ESP32_PINMAP_H

#include <stdint.h>

#include "thermal_config.h"   /* THERMAL_MAX_SENSORS, THERMAL_MAX_ACTUATORS */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int onewire_gpio;        /* shared bus -- same value for every slot */
} esp32_sensor_pin_t;

typedef struct {
    int      pwm_gpio;
    int      tach_gpio;
    uint32_t pwm_freq_hz;    /* shared across slots in v1 (LEDC_TIMER_0) */
} esp32_actuator_pin_t;

typedef struct {
    uint8_t              sensor_count;
    esp32_sensor_pin_t   sensors[THERMAL_MAX_SENSORS];
    uint8_t              actuator_count;
    esp32_actuator_pin_t actuators[THERMAL_MAX_ACTUATORS];
} esp32_pinmap_t;

extern const esp32_pinmap_t G_ESP32_PINMAP;

#ifdef __cplusplus
}
#endif

#endif /* ESP32_PINMAP_H */
