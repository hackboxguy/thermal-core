/* platform/esp32_idf/main/esp32_usb_cdc.h
 *
 * Stage 14 14a -- binary TC frame I/O over the ESP32-C3's
 * USB-Serial-JTAG ROM peripheral.
 *
 * The C3 has a hardware USB-Serial-JTAG block (not a software
 * USB-CDC-ACM stack); ESP-IDF binds stdio to it when
 * CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y (set in
 * sdkconfig.defaults since 13a).  In HIL_PERIPHERAL mode we
 * use that same channel to carry binary TC frames between the
 * ESP32 and the Linux daemon.
 *
 * Binary frames and ESP_LOG / printf debug output cannot share
 * the same channel without a synchronizer.  esp32_usb_cdc_init()
 * therefore:
 *
 *   1. Disables stdout line buffering so multi-byte writes
 *      stay atomic (the IDF default is _IOLBF at 128 B, which
 *      would split frames on embedded 0x0A bytes).
 *   2. Suppresses ESP_LOG entirely (esp_log_level_set("*",
 *      ESP_LOG_NONE)) so the boot-time "I (123) cpu_start:
 *      ..." lines don't poison the receive stream.
 *
 * The 14b commit will add an esp32_usb_cdc_read() entry point
 * for the inbound CMD_REQUEST path.
 */
#ifndef ESP32_USB_CDC_H
#define ESP32_USB_CDC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configure stdout for binary TC frame writes.  Disables
 * line buffering and mutes ESP_LOG.  Call once at HIL
 * app_main entry, before any frame is emitted. */
void esp32_usb_cdc_init(void);

/* Write `len` bytes of an already-encoded TC frame to the
 * USB-Serial-JTAG output.  Non-blocking from the caller's
 * perspective (stdio is flushed at the end of the call). */
void esp32_usb_cdc_write(const uint8_t *buf, size_t len);

/* Pull up to `cap` bytes off the USB-Serial-JTAG receive
 * queue into `buf`.  Returns the byte count actually read
 * (0 if nothing pending; never negative).  Non-blocking:
 * `usb_serial_jtag_read_bytes(..., 0 ticks)` returns
 * immediately with whatever the driver's internal ring
 * buffer already holds.  14b uses this to drive the inbound
 * TC frame accumulator in app_main_hil_peripheral. */
size_t esp32_usb_cdc_read(uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ESP32_USB_CDC_H */
