/* platform/linux/telem_wire.h
 *
 * Stage 9 9c — v1 TELEM_SAMPLE / TELEM_EVENT wire encoder.
 *
 * Pure little-endian byte packer.  No allocation, no I/O — the
 * daemon calls these from the telemetry_emit / log_event callbacks
 * to build a fixed-width frame, then send()s it on the UDP socket.
 *
 * Stopgap: Stage 10 will hoist this into the canonical
 * protocol/thermal_wire.c (with CRC, sequence numbers, opcode
 * registry).  Until then v1 keeps it minimal so the daemon emits
 * something observable.
 *
 * Frame layout:
 *
 *   TELEM_SAMPLE (11 bytes, all little-endian)
 *     uint8_t  type = 0x01
 *     uint32_t ts_ms
 *     uint16_t signal_id
 *     int32_t  value
 *
 *   TELEM_EVENT (23 bytes, all little-endian)
 *     uint8_t  type = 0x02
 *     uint32_t ts_ms
 *     uint16_t code
 *     uint32_t a1
 *     uint32_t a2
 *     uint32_t a3
 *     uint32_t a4
 */
#ifndef THERMAL_TELEM_WIRE_H
#define THERMAL_TELEM_WIRE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TELEM_WIRE_TYPE_SAMPLE   0x01u
#define TELEM_WIRE_TYPE_EVENT    0x02u

#define TELEM_WIRE_SAMPLE_LEN    11u
#define TELEM_WIRE_EVENT_LEN     23u

/* Pack a TELEM_SAMPLE frame into `buf`. `buf` must point to at
 * least TELEM_WIRE_SAMPLE_LEN bytes.  Returns the number of bytes
 * written. */
size_t telem_wire_pack_sample(uint8_t *buf,
                              uint32_t ts_ms,
                              uint16_t signal_id,
                              int32_t  value);

/* Pack a TELEM_EVENT frame into `buf`. `buf` must point to at
 * least TELEM_WIRE_EVENT_LEN bytes.  Returns the number of bytes
 * written. */
size_t telem_wire_pack_event(uint8_t *buf,
                             uint32_t ts_ms,
                             uint16_t code,
                             uint32_t a1,
                             uint32_t a2,
                             uint32_t a3,
                             uint32_t a4);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_TELEM_WIRE_H */
