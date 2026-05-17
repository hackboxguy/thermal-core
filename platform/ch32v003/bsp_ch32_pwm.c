/* platform/ch32v003/bsp_ch32_pwm.c
 *
 * Stage 18b SKELETON STUB. Canned no-op implementation so the
 * firmware links and the flash budget can be measured before the
 * real TIM2 PWM driver is built (Stage 18c). The signatures are
 * final; only the bodies are placeholder.
 */
#include "bsp_ch32_pwm.h"

int bsp_ch32_pwm_init(uint8_t slot, int pin, uint32_t freq_hz)
{
    (void)slot;
    (void)pin;
    (void)freq_hz;
    return 0;
}

void bsp_ch32_pwm_set_duty(uint8_t slot, uint8_t duty_0_255)
{
    (void)slot;
    (void)duty_0_255;
}
