/* platform/ch32v003/bsp_ch32_sensor.c
 *
 * Stage 18c -- CH32V003 DS18B20 temperature-sensor BSP. Bit-banged
 * 1-Wire over ch32fun's static_onewire.h. Adapted from the bench
 * firmware's ds18b20_start_conversion / ds18b20_read; the reading
 * is returned in milli-degrees C (the thermal-core sensor unit)
 * rather than the bench's centidegrees.
 *
 * bsp_ch32_sensor_read_mc() is blocking: it kicks off a conversion
 * and waits it out (12-bit DS18B20 conversion <= 750 ms). The
 * STANDALONE control loop's period is sized to absorb that wait.
 */
#include "bsp_ch32_sensor.h"

#include "ch32fun.h"

/* DS18B20 ROM / function commands. */
#define DS_SKIP_ROM  0xCC
#define DS_CONVERT_T 0x44
#define DS_READ_SCR  0xBE

/* The 1-Wire data pin. A variable (not a compile-time macro) so the
 * bus pin comes from the JSON pin map at init time; funPinMode /
 * funDigitalWrite / funDigitalRead all take a runtime pin. */
static int s_onewire_pin = 51;   /* PD3 default */

/* Glue for ch32fun's static_onewire.h (ONEPREFIX one -> oneReset,
 * oneSendByte, oneGetByte, oneCRC8). */
#define ONEPREFIX one
#define DELAY(n)  Delay_Us(n)
#define ONE_INPUT                                                      \
    {                                                                  \
        funPinMode((uint32_t)s_onewire_pin, GPIO_CFGLR_IN_PUPD);       \
        funDigitalWrite((uint32_t)s_onewire_pin, 1);                   \
    }
#define ONE_OUTPUT                                                     \
    {                                                                  \
        funDigitalWrite((uint32_t)s_onewire_pin, 0);                   \
        funPinMode((uint32_t)s_onewire_pin, GPIO_CFGLR_OUT_2Mhz_PP);   \
    }
#define ONE_SET   { funDigitalWrite((uint32_t)s_onewire_pin, 1); }
#define ONE_CLEAR { funDigitalWrite((uint32_t)s_onewire_pin, 0); }
#define ONE_READ  funDigitalRead((uint32_t)s_onewire_pin)
#define ONENEEDCRC8_TABLE 1
#include "static_onewire.h"

int bsp_ch32_sensor_init(int pin, uint8_t expected_count)
{
    (void)expected_count;        /* tiny profile: one sensor */
    s_onewire_pin = pin;
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD;
    ONE_INPUT;                   /* park the bus idle-high */
    return 0;
}

int bsp_ch32_sensor_read_mc(uint8_t slot, int32_t *out_mc)
{
    if (slot != 0 || out_mc == 0) {
        return -1;
    }

    /* Kick off a conversion (SKIP ROM -- a single sensor on the bus). */
    if (oneReset() != 0) {
        return -1;               /* no presence pulse */
    }
    oneSendByte(DS_SKIP_ROM);
    oneSendByte(DS_CONVERT_T);
    Delay_Ms(800);               /* 12-bit conversion <= 750 ms */

    /* Read the 9-byte scratchpad back. */
    if (oneReset() != 0) {
        return -1;
    }
    oneSendByte(DS_SKIP_ROM);
    oneSendByte(DS_READ_SCR);

    uint8_t scr[9];
    for (int i = 0; i < 9; i++) {
        scr[i] = oneGetByte();
    }
    if (oneCRC8(0, scr, 8) != scr[8]) {
        return -2;               /* CRC mismatch */
    }

    /* Scratchpad bytes 0,1 are the signed 16-bit raw value at
     * 1/16 C per LSB; convert to milli-degrees C. */
    int16_t raw = (int16_t)(((uint16_t)scr[1] << 8) | scr[0]);
    *out_mc = ((int32_t)raw * 1000) / 16;
    return 0;
}
