/* platform/esp32_idf/main/esp32_usb_cdc.c
 *
 * See esp32_usb_cdc.h for the contract.  The implementation
 * is intentionally small: ESP-IDF already routes stdio to the
 * USB-Serial-JTAG ROM peripheral via CONFIG_ESP_CONSOLE_USB_
 * SERIAL_JTAG=y, so we just need to reconfigure stdio for raw
 * binary writes and silence ESP_LOG.
 */
#include "esp32_usb_cdc.h"

#include <stdio.h>

#include "esp_log.h"
#include "driver/usb_serial_jtag.h"

void esp32_usb_cdc_init(void)
{
    /* Disable stdout line buffering -- TC binary frames contain
     * arbitrary bytes including 0x0A.  The IDF default _IOLBF at
     * 128 B would otherwise split frames at every newline byte,
     * leaving the host decoder permanently misaligned. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Suppress ESP_LOG entirely.  Boot-time component logs
     * (`I (123) cpu_start: ...`) would otherwise prefix the
     * binary stream and CRC-fail on the host decoder.  ESP_LOG
     * cannot share a channel with binary frames without a
     * frame-level synchronizer, which 14a does not implement. */
    esp_log_level_set("*", ESP_LOG_NONE);
}

void esp32_usb_cdc_write(const uint8_t *buf, size_t len)
{
    if (len == 0) return;
    (void)fwrite(buf, 1, len, stdout);
    (void)fflush(stdout);
}

size_t esp32_usb_cdc_read(uint8_t *buf, size_t cap)
{
    if (cap == 0) return 0;
    /* 0-tick wait: return immediately with whatever bytes the
     * USB-Serial-JTAG driver already has buffered.  The driver
     * is auto-installed by ESP-IDF when
     * CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y (sdkconfig.defaults,
     * Stage 13a), so we don't call usb_serial_jtag_driver_install
     * ourselves. */
    int n = usb_serial_jtag_read_bytes(buf, cap, 0);
    return (n < 0) ? 0 : (size_t)n;
}
