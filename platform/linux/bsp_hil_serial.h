/* platform/linux/bsp_hil_serial.h
 *
 * Stage 14 14c -- HIL_PERIPHERAL serial transport for the Linux
 * daemon.  Sibling to bsp_mock_tmpfs (file-backed) and
 * bsp_socketcan (CAN context).  In HIL mode the daemon owns one
 * serial device (e.g. /dev/ttyACM0) that serves three roles
 * simultaneously:
 *
 *   - INPUT: inbound `TELEM_SAMPLE` TC frames carrying the ESP32
 *     firmware's sensor + tach readings, decoded into a per-slot
 *     sample cache (`bsp_hil_serial_drain`).
 *   - OUTPUT: outbound `CMD_REQUEST` TC frames carrying
 *     `HIL_CMD_SET_PWM_DUTY = 0x8001` after each tick
 *     (`bsp_hil_serial_write_actuator`).
 *   - HOUSEKEEPING: inbound `CMD_ACK` / `CMD_NACK` frames in
 *     response to our writes are decoded + counted in the drain
 *     for visibility; the daemon does not block on ACKs (writes
 *     are eventually-consistent across ticks).
 *
 * PRD §8.3 lines 1053-1062 pins the framing: same `TC` magic +
 * CRC-16/CCITT-FALSE codec as UDP, no second ad-hoc protocol.
 *
 * Signal IDs use `TSIG_HIL_SENSOR_TEMP(slot)` and
 * `TSIG_HIL_TACH_RPM(slot)` from `core/thermal_signals.h`
 * (promoted from provisional defines in 14a/b's ESP32 firmware).
 */
#ifndef BSP_HIL_SERIAL_H
#define BSP_HIL_SERIAL_H

#include <stdint.h>

#include "thermal_config.h"
#include "thermal_core.h"    /* thermal_config_t */
#include "thermal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a `serial:<path>[:baud]` URI and open the device in raw
 * mode with O_NONBLOCK.  Default baud is 115200 (USB-Serial-JTAG
 * ignores baud anyway, but other USB-CDC chips honour it).
 * Returns the fd on success, -1 on parse or open failure (errno
 * propagated). */
int  bsp_hil_serial_open(const char *uri);

void bsp_hil_serial_close(int fd);

/* Pull every byte currently waiting on `fd`, run the sliding-
 * window TC frame scanner, and dispatch each decoded frame:
 *   TELEM_SAMPLE -> update per-signal sample cache.
 *   CMD_ACK      -> increment ack counter (silent).
 *   CMD_NACK     -> increment nack counter + stderr log.
 *   any other    -> drop.
 *
 * Non-blocking.  Safe to call once per tick alongside the UDP
 * drain_commands() loop.  `now_ms` is informational and used
 * only for stderr timestamps on NACK logs. */
void bsp_hil_serial_drain(int fd, uint32_t now_ms);

/* Snapshot-build accessors.  Fill `*out` with the latest cached
 * value for sensor / tach slot N.  If the ESP32 has never sent
 * that slot (or the cache was cleared at startup), `out->valid`
 * is set to 0 -- the core's filter treats this as a missing
 * sample (same semantic as canbus staleness from Stage 11).
 *
 * Returns 0 always.  Out-of-range `slot` populates `*out` with
 * valid=0 and returns 0 (rather than failing the snapshot build,
 * which would halt the daemon). */
int bsp_hil_serial_read_sensor(const thermal_config_t *cfg,
                                uint8_t slot, uint32_t now_ms,
                                thermal_sample_t *out);
int bsp_hil_serial_read_tach(const thermal_config_t *cfg,
                              uint8_t slot, uint32_t now_ms,
                              thermal_sample_t *out);

/* Encode a CMD_REQUEST(HIL_CMD_SET_PWM_DUTY=0x8001, duty) frame
 * and write it to `fd`.  Hand-rolled because the standard
 * `thermal_wire_encode_cmd_request` rejects every command_id
 * outside 0x0001..0x0005 (its switch maps each known ID to a
 * struct-shaped payload from `thermal_command_t.u`); platform-
 * private IDs must be built byte-by-byte.
 *
 * `slot` is currently unused (PRD reserves the per-slot payload
 * shape for future expansion -- the v1 firmware applies the
 * duty to actuator slot 0 only).  Returns the number of bytes
 * written, or -1 on write error. */
int bsp_hil_serial_write_actuator(int fd, uint32_t ts_ms,
                                   uint8_t slot, uint8_t duty_0_255);

#ifdef __cplusplus
}
#endif

#endif /* BSP_HIL_SERIAL_H */
