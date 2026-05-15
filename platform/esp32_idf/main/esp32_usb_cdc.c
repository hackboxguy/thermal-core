/* platform/esp32_idf/main/esp32_usb_cdc.c
 *
 * See esp32_usb_cdc.h for the contract.
 *
 * Both directions go through the interrupt-driven
 * `usb_serial_jtag` driver.  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
 * only wires a *polled* console-output path -- it does NOT
 * install this driver.  `usb_serial_jtag_{read,write}_bytes`
 * both dereference the driver object (`p_usb_serial_jtag_obj`);
 * without an explicit `usb_serial_jtag_driver_install` that
 * object is NULL and `read_bytes` faults (Load access fault).
 */
#include "esp32_usb_cdc.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "driver/usb_serial_jtag.h"

void esp32_usb_cdc_init(void)
{
    /* Mute ESP_LOG: binary TC frames must not interleave with
     * ASCII log lines on the shared USB-Serial-JTAG channel.
     * (Boot-time logs printed before app_main already went out
     * over the polled console path; from here on the driver
     * owns the peripheral and only binary frames flow.) */
    esp_log_level_set("*", ESP_LOG_NONE);

    /* Install the interrupt-driven USB-Serial-JTAG driver -- the
     * prerequisite for usb_serial_jtag_{read,write}_bytes.  The
     * default config gives 256-byte TX + RX ring buffers, ample
     * for the 17/18-byte TC frames exchanged in HIL mode. */
    usb_serial_jtag_driver_config_t cfg =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    (void)usb_serial_jtag_driver_install(&cfg);
}

void esp32_usb_cdc_write(const uint8_t *buf, size_t len)
{
    if (len == 0) return;
    /* Block until the whole frame is queued in the driver's TX
     * ring -- a finite timeout could queue a partial frame and
     * desync the host decoder.  The HIL daemon drains the
     * channel continuously, so the ring never stays full. */
    (void)usb_serial_jtag_write_bytes(buf, len, portMAX_DELAY);
}

size_t esp32_usb_cdc_read(uint8_t *buf, size_t cap)
{
    if (cap == 0) return 0;
    /* 0-tick wait: return immediately with whatever bytes the
     * driver's RX ring already holds (non-blocking drain). */
    int n = usb_serial_jtag_read_bytes(buf, cap, 0);
    return (n < 0) ? 0 : (size_t)n;
}
