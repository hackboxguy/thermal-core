/* platform/ch32v003/bsp_ch32_tach.c
 *
 * Stage 18b SKELETON STUB. Canned no-op implementation so the
 * firmware links and the flash budget can be measured before the
 * real EXTI tach driver is built (Stage 18c). The signatures are
 * final; only the bodies are placeholder.
 */
#include "bsp_ch32_tach.h"

int bsp_ch32_tach_init(uint8_t slot, int pin)
{
    (void)slot;
    (void)pin;
    return 0;
}

uint32_t bsp_ch32_tach_read_ticks_delta(uint8_t slot)
{
    (void)slot;
    return 0;
}
