/* platform/esp32_idf/main/bsp_esp32_sensor.c
 *
 * DS18B20 wrapper.  See bsp_esp32_sensor.h for the contract.
 */
#include "bsp_esp32_sensor.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "onewire_bus.h"
#include "ds18b20.h"

static const char *TAG = "bsp_sensor";

#define CONVERSION_WAIT_MS  800   /* 12-bit conversion <= 750 ms */

static ds18b20_device_handle_t s_ds = NULL;

int bsp_esp32_sensor_init(int gpio_num)
{
    onewire_bus_handle_t bus = NULL;
    onewire_bus_config_t bus_cfg     = { .bus_gpio_num = gpio_num };
    onewire_bus_rmt_config_t rmt_cfg = { .max_rx_bytes = 10 };
    if (onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "onewire_new_bus_rmt failed");
        return -1;
    }

    onewire_device_iter_handle_t iter = NULL;
    if (onewire_new_device_iter(bus, &iter) != ESP_OK) {
        ESP_LOGE(TAG, "onewire_new_device_iter failed");
        return -1;
    }

    onewire_device_t dev;
    int rc = -1;
    if (onewire_device_iter_get_next(iter, &dev) == ESP_OK) {
        ds18b20_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        if (ds18b20_new_device(&dev, &cfg, &s_ds) == ESP_OK) {
            ds18b20_set_resolution(s_ds, DS18B20_RESOLUTION_12B);
            ESP_LOGI(TAG, "DS18B20 found on GPIO%d, ROM=0x%016" PRIx64,
                      gpio_num, dev.address);
            rc = 0;
        } else {
            ESP_LOGE(TAG, "ds18b20_new_device failed");
        }
    } else {
        ESP_LOGE(TAG,
                  "no DS18B20 on 1-Wire bus (check 4.7k pull-up on GPIO%d)",
                  gpio_num);
    }
    onewire_del_device_iter(iter);
    return rc;
}

int bsp_esp32_sensor_read_mc(int32_t *out_mc)
{
    if (!s_ds || !out_mc) return -1;

    if (ds18b20_trigger_temperature_conversion(s_ds) != ESP_OK) {
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(CONVERSION_WAIT_MS));

    float temp_c = 0.0f;
    if (ds18b20_get_temperature(s_ds, &temp_c) != ESP_OK) {
        return -1;
    }

    /* Convert to millicelsius int32 -- the BSP's only float-to-int
     * boundary.  Round-to-nearest avoids systematic underbias from
     * floor-truncation toward zero (which would matter for
     * boundary trips). */
    float scaled = temp_c * 1000.0f;
    if (scaled >= 0.0f) scaled += 0.5f; else scaled -= 0.5f;
    *out_mc = (int32_t)scaled;
    return 0;
}
