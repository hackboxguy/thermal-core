/* platform/ch32v003/bsp_ch32_tach.c
 *
 * Stage 18c -- CH32V003 fan-tachometer BSP. Counts falling edges
 * on EXTI line 0. Adapted from the bench firmware's tach_init +
 * EXTI7_0_IRQHandler.
 *
 * EXTI line 0 is hardwired to pin 0 of the selected port, so the
 * pin from the pin map must be Px0 (PD0 on this board). An 8 ms
 * inter-edge filter collapses each real tach pulse's ringing /
 * motor-commutation burst into a single count; the core converts
 * the per-period tick delta to RPM.
 */
#include "bsp_ch32_tach.h"

#include "ch32fun.h"

#define TACH_MIN_EDGE_MS 8

static volatile uint32_t s_tach_count = 0;
static uint32_t          s_tach_last  = 0;

void EXTI7_0_IRQHandler(void) __attribute__((interrupt));
void EXTI7_0_IRQHandler(void)
{
    static uint32_t last_tick = 0;
    if (EXTI->INTFR & EXTI_Line0) {
        EXTI->INTFR = EXTI_Line0;            /* acknowledge */
        uint32_t now = SysTick->CNT;
        if ((uint32_t)(now - last_tick) >=
                Ticks_from_Ms(TACH_MIN_EDGE_MS)) {
            last_tick = now;
            s_tach_count++;
        }
    }
}

int bsp_ch32_tach_init(uint8_t slot, int pin)
{
    if (slot != 0) {
        return -1;              /* tiny profile: one actuator */
    }

    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO;

    /* Input with pull-up (ODR = 1): the tach line is open-collector,
     * idle-high. */
    funPinMode((uint32_t)pin, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite((uint32_t)pin, 1);

    AFIO->EXTICR |= AFIO_EXTICR_EXTI0_PD;    /* route EXTI0 to port D */
    EXTI->INTENR |= EXTI_INTENR_MR0;         /* unmask line 0 */
    EXTI->FTENR  |= EXTI_FTENR_TR0;          /* falling-edge trigger */
    NVIC_EnableIRQ(EXTI7_0_IRQn);
    return 0;
}

uint32_t bsp_ch32_tach_read_ticks_delta(uint8_t slot)
{
    if (slot != 0) {
        return 0;
    }
    uint32_t now   = s_tach_count;           /* volatile snapshot */
    uint32_t delta = now - s_tach_last;
    s_tach_last = now;
    return delta;
}
