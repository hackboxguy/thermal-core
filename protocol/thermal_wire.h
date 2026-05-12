/* protocol/thermal_wire.h
 *
 * Stage 10 10a — canonical "TC" binary wire codec.
 *
 * Encodes / decodes the PRD §7.2 frame:
 *
 *   u8  magic[2] = "TC"
 *   u8  version  = 1
 *   u8  opcode
 *   u16 seq           (little-endian)
 *   u16 payload_len   (little-endian)
 *   u32 ts_ms         (little-endian)
 *   u8  payload[payload_len]
 *   u16 crc16         (little-endian, optional per transport)
 *
 * Same framing carries both telemetry (device → host) and
 * commands (host → device + ACK/NACK back); the opcode field
 * distinguishes.
 *
 * `protocol/` is portable C99 with no platform / heap / syscall
 * deps.  Daemons (Linux, ESP32, RH850, ...) link `core/` +
 * `protocol/` to talk to the host tooling.  `core/` itself does
 * not depend on `protocol/`.
 */
#ifndef THERMAL_PROTOCOL_WIRE_H
#define THERMAL_PROTOCOL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "thermal_commands.h"
#include "thermal_wire_opcodes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decoded frame view; payload still points into the caller's
 * input buffer.  Caller must keep the buffer alive while the
 * frame view is in use. */
typedef struct {
    uint16_t              seq;
    uint32_t              ts_ms;
    thermal_wire_opcode_t opcode;
    const uint8_t        *payload;
    size_t                payload_len;
    int                   crc_enabled;   /* 1 if frame included CRC */
} thermal_wire_frame_t;

/* === Encoders =====================================================
 * Each returns the total written byte count on success
 * (HEADER_LEN + payload_len + (crc_enabled ? CRC_LEN : 0)), or a
 * negative thermal_wire_status_t on too-small buffer / over-cap.
 *
 * `crc_enabled` controls whether the trailing CRC-16/CCITT-FALSE
 * is computed and appended (PRD §7.2 line 921: required on serial
 * / CAN, optional on loopback UDP).
 */

int thermal_wire_encode_telem_sample(uint8_t *buf, size_t buf_sz,
                                      uint16_t seq, uint32_t ts_ms,
                                      uint16_t signal_id,
                                      uint16_t flags,
                                      int32_t  value,
                                      int crc_enabled);

int thermal_wire_encode_telem_event(uint8_t *buf, size_t buf_sz,
                                     uint16_t seq, uint32_t ts_ms,
                                     uint16_t event_code,
                                     uint32_t a1, uint32_t a2,
                                     uint32_t a3, uint32_t a4,
                                     int crc_enabled);

/* Encodes the `command_id` + per-command-ID payload from PRD §7.5
 * lines 968-972 by reading the matching `thermal_command_t.u`
 * union member. */
int thermal_wire_encode_cmd_request(uint8_t *buf, size_t buf_sz,
                                     uint16_t seq, uint32_t ts_ms,
                                     const thermal_command_t *cmd,
                                     int crc_enabled);

int thermal_wire_encode_cmd_ack(uint8_t *buf, size_t buf_sz,
                                 uint16_t seq, uint32_t ts_ms,
                                 uint16_t request_seq,
                                 uint16_t status,
                                 uint32_t detail_code,
                                 int crc_enabled);

int thermal_wire_encode_cmd_nack(uint8_t *buf, size_t buf_sz,
                                  uint16_t seq, uint32_t ts_ms,
                                  uint16_t request_seq,
                                  uint16_t status,
                                  uint32_t detail_code,
                                  int crc_enabled);

/* === Outer-frame decoder =========================================
 * Reads the header, validates magic / version / payload_len <
 * receive_cap / opcode-in-range / (optional) CRC; populates
 * `out`.  Does NOT decode the per-opcode payload — call one of
 * the payload decoders below using the resulting frame view.
 *
 * Returns THERMAL_WIRE_OK on success, otherwise a negative
 * thermal_wire_status_t.
 */
int thermal_wire_decode_frame(const uint8_t *buf, size_t buf_sz,
                               size_t receive_cap,
                               int crc_enabled,
                               thermal_wire_frame_t *out);

/* === Per-opcode payload decoders =================================
 * Each returns THERMAL_WIRE_OK or THERMAL_WIRE_ERR_BAD_PAYLOAD
 * (payload length / opcode-mismatch).
 */

int thermal_wire_decode_telem_sample(const thermal_wire_frame_t *frame,
                                      uint16_t *signal_id,
                                      uint16_t *flags,
                                      int32_t  *value);

int thermal_wire_decode_telem_event(const thermal_wire_frame_t *frame,
                                     uint16_t *event_code,
                                     uint32_t *a1, uint32_t *a2,
                                     uint32_t *a3, uint32_t *a4);

int thermal_wire_decode_cmd_request(const thermal_wire_frame_t *frame,
                                     thermal_command_t *cmd);

/* CMD_ACK and CMD_NACK share an identical payload shape; this one
 * decoder accepts either opcode.  Callers distinguish using
 * `frame->opcode`. */
int thermal_wire_decode_cmd_ack_or_nack(const thermal_wire_frame_t *frame,
                                         uint16_t *request_seq,
                                         uint16_t *status,
                                         uint32_t *detail_code);

/* === CRC helper (exposed for tests + 10d fuzz harness) =========== */
uint16_t thermal_wire_crc16(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_PROTOCOL_WIRE_H */
