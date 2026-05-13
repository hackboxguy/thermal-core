/* platform/esp32_idf/main/bsp_esp32_pwm.h
 *
 * Stage 13 13a -- thin LEDC wrapper for a single PWM-driven fan.
 *
 * Wraps the ESP-IDF `ledc_*` API at LEDC_LOW_SPEED_MODE +
 * LEDC_TIMER_0 + LEDC_CHANNEL_0 with 8-bit duty resolution
 * (0..255).  This matches what the user's existing
 * esp32-thermal-core firmware drives the Noctua NF-A8 with.
 *
 * 13b will wrap this into a `thermal_actuator_cmd_t` apply path
 * inside `thermal_core_step()`.  No float at the BSP boundary.
 */
#ifndef BSP_ESP32_PWM_H
#define BSP_ESP32_PWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the LEDC timer + channel for an 8-bit PWM on
 * `gpio_num` at `freq_hz`.  Typical: GPIO 4, 25 000 Hz (Noctua
 * spec).  Returns 0 on success, -1 if the IDF calls fail. */
int bsp_esp32_pwm_init(int gpio_num, uint32_t freq_hz);

/* Apply a duty value 0..255 to the PWM channel.  No-op if
 * `_init` was not called successfully. */
void bsp_esp32_pwm_set_duty(uint8_t duty_0_255);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ESP32_PWM_H */
