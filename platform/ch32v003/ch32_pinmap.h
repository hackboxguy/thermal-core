/* platform/ch32v003/ch32_pinmap.h
 *
 * Stage 18 -- JSON-driven pin map for the CH32V003 STANDALONE
 * firmware. The CH32V003 counterpart of platform/esp32_idf/main/
 * esp32_pinmap.h: json2static.py emits `const ch32_pinmap_t
 * G_CH32_PINMAP` from the JSON's `mcu_pinmap` section (run with
 * --pinmap-prefix ch32), alongside `G_THERMAL_CFG`.
 *
 * GPIO encoding: the pin fields hold ch32fun pin ids, i.e.
 * (port_index << 4) | pin_number -- PD0 = 48, PD3 = 51, PD4 = 52
 * (see ch32fun's GpioOf()). The BSP wrappers pass these straight
 * to funPinMode()/funDigitalWrite().
 *
 * v1 scope: one DS18B20 on a single 1-Wire bus, one 4-wire fan
 * (PWM + tach). The tiny build profile (PRD Appendix D.3) caps
 * THERMAL_MAX_SENSORS / THERMAL_MAX_ACTUATORS at 1, so the arrays
 * below are single-slot in practice; the slot shape mirrors the
 * ESP32 pinmap so the two ports share one json2static path.
 */
#ifndef CH32_PINMAP_H
#define CH32_PINMAP_H

#include <stdint.h>

#include "thermal_config.h"   /* THERMAL_MAX_SENSORS, THERMAL_MAX_ACTUATORS */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int onewire_gpio;        /* DS18B20 1-Wire data pin (ch32fun pin id) */
} ch32_sensor_pin_t;

typedef struct {
    int      pwm_gpio;       /* TIM2 PWM output pin (ch32fun pin id) */
    int      tach_gpio;      /* EXTI tach input pin (ch32fun pin id) */
    uint32_t pwm_freq_hz;    /* PWM carrier frequency */
} ch32_actuator_pin_t;

typedef struct {
    uint8_t             sensor_count;
    ch32_sensor_pin_t   sensors[THERMAL_MAX_SENSORS];
    uint8_t             actuator_count;
    ch32_actuator_pin_t actuators[THERMAL_MAX_ACTUATORS];
} ch32_pinmap_t;

extern const ch32_pinmap_t G_CH32_PINMAP;

#ifdef __cplusplus
}
#endif

#endif /* CH32_PINMAP_H */
