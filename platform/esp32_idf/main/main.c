/* platform/esp32_idf/main/main.c
 *
 * Stage 13 13a -- ESP32-C3 STANDALONE app_main scaffold.
 *
 * Reproduces the user's existing esp32-thermal-core firmware
 * behaviour (linear temp -> duty + 3% hysteresis) but in the
 * canonical project layout: three BSP wrappers + a thin
 * app_main.  13b will replace the inline policy block (marked
 * below) with `thermal_core_step()` against a static
 * thermal_config_t generated from configs/minimal-1zone-1fan.json
 * by tools/json2static.py.
 *
 * All math is integer Q16.16 -- no float in app_main, no FPU
 * dependency.  The DS18B20 driver returns float internally;
 * `bsp_esp32_sensor_read_mc` is the only file that crosses that
 * boundary, converting float Celsius into int32 millicelsius.
 *
 * Hardware (matches the user's working board):
 *   GPIO 4 -- fan PWM out (25 kHz, 3.3 V)
 *   GPIO 5 -- fan tach in (10k pull-up to 3.3 V)
 *   GPIO 6 -- DS18B20 1-Wire data (4.7k pull-up to 3.3 V)
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp_esp32_pwm.h"
#include "bsp_esp32_tach.h"
#include "bsp_esp32_sensor.h"

#define PIN_FAN_PWM     4
#define PIN_FAN_TACH    5
#define PIN_ONEWIRE     6

#define PWM_FREQ_HZ     25000

/* Q16.16 fixed-point constants -- no float in app_main. */
#define Q16_ONE         65536
#define DUTY_IDLE_Q16   ((Q16_ONE * 20) / 100)     /* 0.20 */
#define DUTY_FULL_Q16   Q16_ONE                     /* 1.00 */
#define DUTY_HYST_Q16   ((Q16_ONE * 3)  / 100)     /* 0.03 */

#define TEMP_MIN_MC     30000   /* at/below -> idle duty */
#define TEMP_MAX_MC     60000   /* at/above -> full duty */
#define TEMP_BAD_MC     INT32_MIN

#define LOOP_PERIOD_MS  1000

static const char *TAG = "thermal";

/* Linear temp_mc -> Q16.16 duty fraction, then to a 0..255
 * PWM register value.  Behaviour matches the user's existing
 * temp_to_duty(float) but with int64 intermediates so
 * overflow is impossible across the full temp range. */
static uint8_t temp_to_pwm(int32_t temp_mc)
{
    int32_t duty_q16;
    if (temp_mc == TEMP_BAD_MC || temp_mc >= TEMP_MAX_MC) {
        duty_q16 = DUTY_FULL_Q16;
    } else if (temp_mc <= TEMP_MIN_MC) {
        duty_q16 = DUTY_IDLE_Q16;
    } else {
        int64_t k_q16 = ((int64_t)(temp_mc - TEMP_MIN_MC) * Q16_ONE) /
                        (int64_t)(TEMP_MAX_MC - TEMP_MIN_MC);
        duty_q16 = DUTY_IDLE_Q16 +
                   (int32_t)((k_q16 * (DUTY_FULL_Q16 - DUTY_IDLE_Q16))
                              / Q16_ONE);
    }
    int64_t pwm = ((int64_t)duty_q16 * 255) / Q16_ONE;
    if (pwm < 0)   pwm = 0;
    if (pwm > 255) pwm = 255;
    return (uint8_t)pwm;
}

void app_main(void)
{
    ESP_LOGI(TAG, "thermal-core ESP32-C3 STANDALONE (Stage 13 13a)");

    if (bsp_esp32_pwm_init(PIN_FAN_PWM, PWM_FREQ_HZ) != 0) return;
    if (bsp_esp32_tach_init(PIN_FAN_TACH)            != 0) return;
    int sensor_ok = (bsp_esp32_sensor_init(PIN_ONEWIRE) == 0);

    /* Start at idle duty so the fan spins up cleanly. */
    uint8_t pwm_applied = temp_to_pwm(TEMP_MIN_MC);
    bsp_esp32_pwm_set_duty(pwm_applied);

    for (;;) {
        int64_t t0 = esp_timer_get_time();

        /* ---- Read sensors (BSP) ---- */
        int32_t temp_mc = TEMP_BAD_MC;
        if (sensor_ok) {
            if (bsp_esp32_sensor_read_mc(&temp_mc) != 0) {
                temp_mc = TEMP_BAD_MC;
            }
        } else {
            /* No sensor: pace the loop manually so the tach
             * window stays ~1 s. */
            vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS - 200));
        }

        /* ---- POLICY (13b will replace with thermal_core_step) ---- */
        uint8_t pwm_target = temp_to_pwm(temp_mc);
        int diff = (int)pwm_target - (int)pwm_applied;
        if (diff < 0) diff = -diff;
        int hyst_pwm = (DUTY_HYST_Q16 * 255) / Q16_ONE;   /* ~7 */
        if (diff >= hyst_pwm) {
            pwm_applied = pwm_target;
            bsp_esp32_pwm_set_duty(pwm_applied);
        }
        /* ---- END policy ---- */

        /* Pad to exactly LOOP_PERIOD_MS so the tach window is
         * consistent. */
        int64_t elapsed_us = esp_timer_get_time() - t0;
        int64_t remaining_us = (int64_t)LOOP_PERIOD_MS * 1000 - elapsed_us;
        if (remaining_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(remaining_us / 1000));
        }

        uint32_t ticks = bsp_esp32_tach_read_ticks_delta();
        if (temp_mc == TEMP_BAD_MC) {
            printf("T=  ERR    duty=%3u/255 (%3u%%)  tach=%4" PRIu32
                   " ticks/s  (~%4" PRIu32 " RPM)\n",
                   (unsigned)pwm_applied,
                   (unsigned)((pwm_applied * 100u) / 255u),
                   ticks, ticks * 30u);
        } else {
            printf("T=%6.2f C  duty=%3u/255 (%3u%%)  tach=%4" PRIu32
                   " ticks/s  (~%4" PRIu32 " RPM)\n",
                   (double)temp_mc / 1000.0,
                   (unsigned)pwm_applied,
                   (unsigned)((pwm_applied * 100u) / 255u),
                   ticks, ticks * 30u);
        }
    }
}
