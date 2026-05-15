/* platform/esp32_idf/main/bsp_esp32_tach.c
 *
 * Slot-indexed tach edge counter.  See bsp_esp32_tach.h for the
 * contract.
 */
#include "bsp_esp32_tach.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "thermal_config.h"   /* THERMAL_MAX_ACTUATORS */

static const char *TAG = "bsp_tach";

#define INTER_EDGE_MIN_US  1000   /* 1 ms; rejects EMI bursts. */

/* Per-slot counter + last-edge-timestamp.  ISR is shared but
 * receives the slot via its `arg` pointer (cast at
 * gpio_isr_handler_add time). */
static volatile uint32_t s_tach_count[THERMAL_MAX_ACTUATORS] = { 0 };
static volatile int64_t  s_last_us[THERMAL_MAX_ACTUATORS]    = { 0 };
static uint8_t           s_initialised[THERMAL_MAX_ACTUATORS] = { 0 };
static uint8_t           s_isr_service_installed = 0;

static void IRAM_ATTR tach_isr(void *arg)
{
    uintptr_t slot_u = (uintptr_t)arg;
    if (slot_u >= THERMAL_MAX_ACTUATORS) return;
    int64_t now = esp_timer_get_time();
    if (now - s_last_us[slot_u] < INTER_EDGE_MIN_US) return;
    s_last_us[slot_u] = now;
    s_tach_count[slot_u]++;
}

int bsp_esp32_tach_init(uint8_t slot, int gpio_num)
{
    if (slot >= THERMAL_MAX_ACTUATORS) {
        ESP_LOGE(TAG, "slot %u out of range (max %u)",
                 (unsigned)slot, (unsigned)THERMAL_MAX_ACTUATORS);
        return -1;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed (slot %u)", (unsigned)slot);
        return -1;
    }

    /* gpio_install_isr_service is idempotent across BSPs.  First
     * call wins; subsequent calls return INVALID_STATE which
     * we treat as success. */
    if (!s_isr_service_installed) {
        esp_err_t rc = gpio_install_isr_service(0);
        if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "gpio_install_isr_service failed: %d", (int)rc);
            return -1;
        }
        s_isr_service_installed = 1;
    }

    if (gpio_isr_handler_add(gpio_num, tach_isr,
                              (void *)(uintptr_t)slot) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed (slot %u)", (unsigned)slot);
        return -1;
    }
    s_initialised[slot] = 1;
    ESP_LOGI(TAG, "tach slot %u on GPIO%d (negedge, %d us filter)",
             (unsigned)slot, gpio_num, INTER_EDGE_MIN_US);
    return 0;
}

uint32_t bsp_esp32_tach_read_ticks_delta(uint8_t slot)
{
    if (slot >= THERMAL_MAX_ACTUATORS) return 0;
    if (!s_initialised[slot]) return 0;
    uint32_t snap = s_tach_count[slot];
    s_tach_count[slot] = 0;
    return snap;
}
