/* platform/ch32v003/bsp_ch32_tach.h
 *
 * Stage 18 -- CH32V003 fan-tachometer BSP wrapper. Slot-indexed to
 * mirror the ESP32 BSP shape (platform/esp32_idf/main/
 * bsp_esp32_tach.h). Counts falling edges on an EXTI line; the core
 * converts the per-tick tick delta to RPM.
 */
#ifndef BSP_CH32_TACH_H
#define BSP_CH32_TACH_H

#include <stdint.h>

/* Configure tach edge counting for `slot` on `pin` (ch32fun pin
 * id). Returns 0 on success, non-zero on failure. */
int bsp_ch32_tach_init(uint8_t slot, int pin);

/* Return the tach edge count since the previous call for `slot`,
 * and reset the counter. */
uint32_t bsp_ch32_tach_read_ticks_delta(uint8_t slot);

#endif /* BSP_CH32_TACH_H */
