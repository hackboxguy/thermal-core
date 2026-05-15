/* platform/esp32_idf/main/esp32_usb_cdc.h
 *
 * Binary TC frame I/O over the ESP32-C3's USB-Serial-JTAG
 * peripheral, for HIL_PERIPHERAL mode.
 *
 * The C3 has a hardware USB-Serial-JTAG block (not a software
 * USB-CDC-ACM stack).  Both directions go through the
 * interrupt-driven `usb_serial_jtag` driver, which
 * `esp32_usb_cdc_init()` installs explicitly.
 *
 * Note: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (set in
 * sdkconfig.defaults) only wires a polled console-output path
 * -- it does NOT install the driver.  An earlier revision
 * assumed it did, and `usb_serial_jtag_read_bytes()` faulted
 * on a NULL driver object; `esp32_usb_cdc_init()` now does the
 * `usb_serial_jtag_driver_install()`.
 *
 * `esp32_usb_cdc_init()` also mutes ESP_LOG -- binary frames
 * and ASCII log lines cannot share the channel without a
 * frame-level synchronizer, which this layer does not
 * implement.
 */
#ifndef ESP32_USB_CDC_H
#define ESP32_USB_CDC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the usb_serial_jtag driver + mute ESP_LOG.  Call
 * once at HIL app_main entry, before any frame is read or
 * written. */
void esp32_usb_cdc_init(void);

/* Write `len` bytes of an already-encoded TC frame to the
 * USB-Serial-JTAG output.  Blocks until the whole frame is
 * queued in the driver's TX ring (no partial frames). */
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
