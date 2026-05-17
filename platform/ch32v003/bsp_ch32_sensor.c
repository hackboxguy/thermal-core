/* platform/ch32v003/bsp_ch32_sensor.c
 *
 * Stage 18b SKELETON STUB. Returns a canned 25.000 C reading so the
 * firmware links and the flash budget can be measured before the
 * real bit-banged DS18B20 1-Wire driver is built (Stage 18c). The
 * signatures are final; only the bodies are placeholder.
 */
#include "bsp_ch32_sensor.h"

int bsp_ch32_sensor_init(int pin, uint8_t expected_count)
{
    (void)pin;
    (void)expected_count;
    return 0;
}

int bsp_ch32_sensor_read_mc(uint8_t slot, int32_t *out_mc)
{
    (void)slot;
    if (out_mc == 0) {
        return -1;
    }
    *out_mc = 25000;   /* placeholder: 25.000 C */
    return 0;
}
