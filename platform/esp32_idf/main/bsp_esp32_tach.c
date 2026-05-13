/* platform/esp32_idf/main/bsp_esp32_tach.c
 *
 * Tach edge counter.  See bsp_esp32_tach.h for the contract.
 */
#include "bsp_esp32_tach.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bsp_tach";

#define INTER_EDGE_MIN_US  1000   /* 1 ms; rejects EMI bursts.
                                   * Real Noctua NF-A8 max edge
                                   * rate at 2200 RPM is ~73 Hz
                                   * => 13.7 ms between edges. */

static volatile uint32_t s_tach_count = 0;
static int s_initialised = 0;

static void IRAM_ATTR tach_isr(void *arg)
{
    (void)arg;
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_us < INTER_EDGE_MIN_US) return;
    last_us = now;
    s_tach_count++;
}

int bsp_esp32_tach_init(int gpio_num)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* backup; external 10k still recommended */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed");
        return -1;
    }
    /* gpio_install_isr_service is idempotent across BSPs once
     * INVALID_STATE; treat that as success. */
    esp_err_t rc = gpio_install_isr_service(0);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %d", (int)rc);
        return -1;
    }
    if (gpio_isr_handler_add(gpio_num, tach_isr, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed");
        return -1;
    }
    s_initialised = 1;
    ESP_LOGI(TAG, "tach on GPIO%d (negedge, %d us filter)",
              gpio_num, INTER_EDGE_MIN_US);
    return 0;
}

uint32_t bsp_esp32_tach_read_ticks_delta(void)
{
    if (!s_initialised) return 0;
    /* Atomic read-and-zero: a portable C99 approximation that
     * relies on uint32_t reads being naturally atomic on the C3
     * (single-issue, 32-bit core).  Worst case lost: 1 edge if an
     * ISR fires between the read and the store -- acceptable for
     * a ~75-Hz tach signal sampled at 1 Hz. */
    uint32_t snap = s_tach_count;
    s_tach_count = 0;
    return snap;
}
