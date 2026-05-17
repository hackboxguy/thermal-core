/* platform/ch32v003/bsp_ch32_pwm.h
 *
 * Stage 18 -- CH32V003 fan-PWM BSP wrapper. Slot-indexed to mirror
 * the ESP32 BSP shape (platform/esp32_idf/main/bsp_esp32_pwm.h),
 * though the tiny build profile caps the CH32V003 at one actuator.
 * Drives a 4-wire fan from TIM2 (25 kHz carrier).
 */
#ifndef BSP_CH32_PWM_H
#define BSP_CH32_PWM_H

#include <stdint.h>

/* Configure PWM output for `slot` on `pin` (ch32fun pin id) at
 * `freq_hz`. Returns 0 on success, non-zero on failure. */
int bsp_ch32_pwm_init(uint8_t slot, int pin, uint32_t freq_hz);

/* Set the duty cycle for `slot`, 0..255 (0 = off, 255 = full). */
void bsp_ch32_pwm_set_duty(uint8_t slot, uint8_t duty_0_255);

#endif /* BSP_CH32_PWM_H */
