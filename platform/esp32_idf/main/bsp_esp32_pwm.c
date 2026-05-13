/* platform/esp32_idf/main/bsp_esp32_pwm.c
 *
 * LEDC PWM wrapper.  See bsp_esp32_pwm.h for the contract.
 */
#include "bsp_esp32_pwm.h"

#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "bsp_pwm";

#define PWM_RES_BITS   LEDC_TIMER_8_BIT
#define PWM_MAX_DUTY   ((1 << 8) - 1)

static int s_initialised = 0;

int bsp_esp32_pwm_init(int gpio_num, uint32_t freq_hz)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = PWM_RES_BITS,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&t) != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed");
        return -1;
    }

    ledc_channel_config_t c = {
        .gpio_num   = gpio_num,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&c) != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed");
        return -1;
    }

    s_initialised = 1;
    ESP_LOGI(TAG, "PWM on GPIO%d @ %u Hz, 8-bit",
              gpio_num, (unsigned)freq_hz);
    return 0;
}

void bsp_esp32_pwm_set_duty(uint8_t duty_0_255)
{
    if (!s_initialised) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_0_255);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
