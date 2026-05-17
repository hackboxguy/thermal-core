/* platform/ch32v003/bsp_ch32_pwm.c
 *
 * Stage 18c -- CH32V003 fan-PWM BSP. Drives a 4-wire fan from
 * TIM2 channel 1. Adapted from the bench firmware's pwm_init /
 * pwm_set_pct; the duty interface is 0..255 (the thermal-core
 * actuator unit) rather than a percentage.
 *
 * TIM2_CH1 is hardwired to PD4 on the CH32V003 (no remap); the
 * pin from the pin map must therefore be PD4.
 */
#include "bsp_ch32_pwm.h"

#include "ch32fun.h"

/* CH1CVR value for 100% duty (= ATRLR + 1). Set from the requested
 * carrier frequency at init: 48 MHz / (PSC+1=1) / top = freq. */
static uint16_t s_pwm_top = 1920;   /* 25 kHz default */

int bsp_ch32_pwm_init(uint8_t slot, int pin, uint32_t freq_hz)
{
    if (slot != 0) {
        return -1;              /* tiny profile: one actuator */
    }

    uint32_t top = (freq_hz > 0u) ? (48000000u / freq_hz) : 1920u;
    if (top < 2u) {
        top = 2u;
    }
    s_pwm_top = (uint16_t)top;

    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD;
    RCC->APB1PCENR |= RCC_APB1Periph_TIM2;

    /* PWM output pin: alt-function push-pull. */
    funPinMode((uint32_t)pin, GPIO_CFGLR_OUT_10Mhz_AF_PP);

    /* Reset TIM2 to a known state. */
    RCC->APB1PRSTR |=  RCC_APB1Periph_TIM2;
    RCC->APB1PRSTR &= ~RCC_APB1Periph_TIM2;

    TIM2->PSC   = 0;
    TIM2->ATRLR = (uint16_t)(top - 1u);
    /* CH1: PWM mode 1 (OC1M = 110), output-compare preload enabled. */
    TIM2->CHCTLR1 |= TIM_OC1M_2 | TIM_OC1M_1 | TIM_OC1PE;
    TIM2->CTLR1   |= TIM_ARPE;       /* auto-reload preload */
    TIM2->CCER    |= TIM_CC1E;       /* enable CH1 output */
    TIM2->CH1CVR   = 0;              /* start at 0% */
    TIM2->SWEVGR  |= TIM_UG;         /* latch the preloaded registers */
    TIM2->CTLR1   |= TIM_CEN;        /* start the timer */
    return 0;
}

void bsp_ch32_pwm_set_duty(uint8_t slot, uint8_t duty_0_255)
{
    if (slot != 0) {
        return;
    }
    TIM2->CH1CVR =
        (uint16_t)(((uint32_t)duty_0_255 * s_pwm_top) / 255u);
}
