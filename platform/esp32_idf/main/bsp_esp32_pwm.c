/* platform/esp32_idf/main/bsp_esp32_pwm.c
 *
 * Slot-indexed LEDC PWM wrapper.  See bsp_esp32_pwm.h for the
 * contract.
 */
#include "bsp_esp32_pwm.h"

#include "driver/ledc.h"
#include "esp_log.h"

#include "thermal_config.h"   /* THERMAL_MAX_ACTUATORS */

static const char *TAG = "bsp_pwm";

#define PWM_RES_BITS   LEDC_TIMER_8_BIT

/* LEDC channels assigned per slot: slot 0 -> CHANNEL_0,
 * slot 1 -> CHANNEL_1, etc.  ESP32-C3 has 6 low-speed channels
 * total; THERMAL_MAX_ACTUATORS = 2 fits comfortably. */
static const ledc_channel_t s_channel_for_slot[] = {
    LEDC_CHANNEL_0,
    LEDC_CHANNEL_1,
};

static uint8_t s_initialised[THERMAL_MAX_ACTUATORS] = { 0 };
static uint8_t s_timer_configured = 0;

int bsp_esp32_pwm_init(uint8_t slot, int gpio_num, uint32_t freq_hz)
{
    if (slot >= THERMAL_MAX_ACTUATORS) {
        ESP_LOGE(TAG, "slot %u out of range (max %u)",
                 (unsigned)slot, (unsigned)THERMAL_MAX_ACTUATORS);
        return -1;
    }

    /* Configure the shared timer once; subsequent slots inherit it. */
    if (!s_timer_configured) {
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
        s_timer_configured = 1;
    }

    ledc_channel_config_t c = {
        .gpio_num   = gpio_num,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = s_channel_for_slot[slot],
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&c) != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed (slot %u)", (unsigned)slot);
        return -1;
    }

    s_initialised[slot] = 1;
    ESP_LOGI(TAG, "PWM slot %u on GPIO%d @ %u Hz, 8-bit",
             (unsigned)slot, gpio_num, (unsigned)freq_hz);
    return 0;
}

void bsp_esp32_pwm_set_duty(uint8_t slot, uint8_t duty_0_255)
{
    if (slot >= THERMAL_MAX_ACTUATORS) return;
    if (!s_initialised[slot]) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channel_for_slot[slot], duty_0_255);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channel_for_slot[slot]);
}
