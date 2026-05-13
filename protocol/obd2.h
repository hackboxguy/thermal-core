/* protocol/obd2.h
 *
 * Stage 11 11a — portable OBD-II Service 01 codec.
 *
 * Encodes / decodes the 8-byte CAN payload for OBD-II diagnostic
 * requests and responses per PRD §6.1:
 *
 *   Request  (CAN ID 0x7DF functional / 0x7E0 physical, DLC 8):
 *     byte 0 = 0x02            ISO-TP single-frame, payload length 2
 *     byte 1 = 0x01            Service 01: show current data
 *     byte 2 = <pid>           e.g. 0x0D for vehicle speed (km/h)
 *     bytes 3..7 = 0x55        ISO 15765-2 recommended padding
 *
 *   Response (CAN ID 0x7E8 from ECU, DLC 8, single-byte PID):
 *     byte 0 = 0x03            payload length 3
 *     byte 1 = 0x41            Service 01 response (0x01 | 0x40)
 *     byte 2 = <pid>           echoed
 *     byte 3 = <value>         the byte the BSP wants
 *     bytes 4..7 = padding     decoder ignores
 *
 *   Negative response (single-frame):
 *     byte 0 = 0x03
 *     byte 1 = 0x7F            negative response identifier
 *     byte 2 = 0x01            service that was rejected
 *     byte 3 = <NRC>           e.g. 0x12 subFunctionNotSupported
 *
 * v1 covers only single-byte PIDs (PRD §6.1 line 821).  Multi-frame
 * ISO-TP (CF / FF flow-control) is out of scope: the decoder
 * rejects multi-frame PCI bytes (0x10..0x2F) as ERR_BAD_SHAPE.
 *
 * The CAN ID + transport (SocketCAN, TWAI, ...) are the caller's
 * responsibility — this codec sees only the 8-byte payload.
 *
 * `protocol/` is portable C99 with no heap and no platform deps:
 * the Linux SocketCAN BSP (11b) and the ESP32 TWAI driver
 * (Stage 13) both link this module.  `core/` has no dependency on
 * `protocol/` in either direction.
 */
#ifndef THERMAL_PROTOCOL_OBD2_H
#define THERMAL_PROTOCOL_OBD2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === CAN IDs (PRD §6.1) ===================================== */

#define OBD2_REQUEST_ID_FUNCTIONAL  0x7DFu  /* broadcast Mode 01 */
#define OBD2_REQUEST_ID_PHYSICAL    0x7E0u  /* ECU 0 physical    */
#define OBD2_RESPONSE_ID_ECU        0x7E8u  /* ECU 0 response    */

/* === Service + PID constants ================================ */

#define OBD2_SERVICE_01_CURRENT_DATA    0x01u
#define OBD2_SERVICE_01_RESPONSE        (0x40u | OBD2_SERVICE_01_CURRENT_DATA)
#define OBD2_NEGATIVE_RESPONSE          0x7Fu
#define OBD2_PID_VEHICLE_SPEED          0x0Du  /* PRD §6.1 line 814 */

/* === Frame width ============================================ */

#define OBD2_FRAME_DLC              8u

/* Single-frame ISO-TP PCI payload-length nibble bounds.  Bytes
 * with the high nibble >= 1 are multi-frame and rejected. */
#define OBD2_PCI_SINGLE_MAX_NIBBLE  0x0Fu

/* === Codec status =========================================== */

typedef enum {
    OBD2_OK                       =  0,
    OBD2_ERR_BAD_SHAPE            = -1,  /* in_len != 8 or multi-frame PCI */
    OBD2_ERR_WRONG_PCI_LEN        = -2,  /* PCI length field disagrees with response */
    OBD2_ERR_NEGATIVE_RESPONSE    = -3,  /* response service byte == 0x7F (NRC) */
    OBD2_ERR_WRONG_SERVICE        = -4,  /* response service != request + 0x40 */
    OBD2_ERR_WRONG_PID            = -5   /* response PID echo != expected */
} obd2_status_t;

/* === Encoders =============================================== */

/* Build the 8-byte payload for a Service 01 single-byte PID
 * request.  `out` must point to at least OBD2_FRAME_DLC bytes;
 * the caller transmits these bytes on CAN ID
 * OBD2_REQUEST_ID_FUNCTIONAL (or _PHYSICAL).  No return value —
 * the caller already owns the buffer and the only failure mode
 * (NULL out) is a caller bug, not a wire fault. */
void obd2_encode_request_byte(uint8_t pid, uint8_t out[OBD2_FRAME_DLC]);

/* === Decoders =============================================== */

/* Validate an OBD-II Service 01 single-byte-PID response and
 * extract the value byte.
 *
 * `in` is the raw 8-byte CAN payload as delivered by the
 * transport.  `in_len` must equal OBD2_FRAME_DLC; shorter inputs
 * return ERR_BAD_SHAPE so the BSP can't accidentally consume a
 * truncated frame.
 *
 * `expected_pid` is the PID the caller requested, echoed back by
 * the ECU at byte 2.  A mismatch is ERR_WRONG_PID and the BSP
 * should drop the frame (could be a stale response for a
 * different PID that we requested earlier).
 *
 * On success, `*value_out` receives byte 3 of the payload and the
 * function returns OBD2_OK.  On any failure, `*value_out` is
 * unchanged; check the returned obd2_status_t for the reason. */
int obd2_decode_response_byte(const uint8_t *in, size_t in_len,
                              uint8_t expected_pid,
                              uint8_t *value_out);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_PROTOCOL_OBD2_H */
