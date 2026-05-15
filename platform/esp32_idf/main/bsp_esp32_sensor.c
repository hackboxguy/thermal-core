/* platform/esp32_idf/main/bsp_esp32_sensor.c
 *
 * Multi-device DS18B20 wrapper.  See bsp_esp32_sensor.h for the
 * contract.
 */
#include "bsp_esp32_sensor.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "onewire_bus.h"
#include "ds18b20.h"

#include "thermal_config.h"   /* THERMAL_MAX_SENSORS */

static const char *TAG = "bsp_sensor";

#define CONVERSION_WAIT_MS  800   /* 12-bit conversion <= 750 ms */

static ds18b20_device_handle_t s_devs[THERMAL_MAX_SENSORS] = { NULL };
static uint8_t                 s_dev_count = 0;

int bsp_esp32_sensor_init(int gpio_num, uint8_t expected_count)
{
    if (expected_count == 0 || expected_count > THERMAL_MAX_SENSORS) {
        ESP_LOGE(TAG, "expected_count %u out of range [1, %u]",
                 (unsigned)expected_count,
                 (unsigned)THERMAL_MAX_SENSORS);
        return -1;
    }

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

    /* Enumerate devices in driver-deterministic order (ROM-sorted
     * by the espressif/ds18b20 component).  Stop after we have
     * exactly `expected_count` -- having more devices on the bus
     * than the config declares is a wiring mistake we want to
     * surface, but we don't actively probe for excess. */
    s_dev_count = 0;
    onewire_device_t dev;
    while (s_dev_count < expected_count
            && onewire_device_iter_get_next(iter, &dev) == ESP_OK) {
        ds18b20_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        ds18b20_device_handle_t handle = NULL;
        if (ds18b20_new_device(&dev, &cfg, &handle) == ESP_OK) {
            ds18b20_set_resolution(handle, DS18B20_RESOLUTION_12B);
            s_devs[s_dev_count] = handle;
            ESP_LOGI(TAG,
                     "DS18B20 slot %u on GPIO%d, ROM=0x%016" PRIx64,
                     (unsigned)s_dev_count, gpio_num, dev.address);
            s_dev_count++;
        } else {
            ESP_LOGW(TAG, "ds18b20_new_device failed for ROM=0x%016" PRIx64,
                     dev.address);
        }
    }
    onewire_del_device_iter(iter);

    if (s_dev_count != expected_count) {
        ESP_LOGE(TAG,
                 "expected %u DS18B20 devices, found %u on GPIO%d "
                 "(check 4.7k pull-up + parasite-power wiring)",
                 (unsigned)expected_count,
                 (unsigned)s_dev_count, gpio_num);
        return -1;
    }
    return 0;
}

int bsp_esp32_sensor_read_mc(uint8_t slot, int32_t *out_mc)
{
    if (slot >= THERMAL_MAX_SENSORS) return -1;
    if (!s_devs[slot] || !out_mc) return -1;

    /* Per-slot trigger: the espressif/ds18b20 driver does not
     * expose a skip-rom convert helper, so we pay ~800 ms per
     * sensor.  For 2 sensors at the existing 1 Hz refresh
     * cadence in main.c this is fine; if it becomes a problem,
     * a future commit can add a skip-rom helper. */
    if (ds18b20_trigger_temperature_conversion(s_devs[slot]) != ESP_OK) {
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(CONVERSION_WAIT_MS));

    float temp_c = 0.0f;
    if (ds18b20_get_temperature(s_devs[slot], &temp_c) != ESP_OK) {
        return -1;
    }

    /* Float -> millicelsius int32.  Round-to-nearest avoids
     * floor-toward-zero bias at trip boundaries. */
    float scaled = temp_c * 1000.0f;
    if (scaled >= 0.0f) scaled += 0.5f; else scaled -= 0.5f;
    *out_mc = (int32_t)scaled;
    return 0;
}
