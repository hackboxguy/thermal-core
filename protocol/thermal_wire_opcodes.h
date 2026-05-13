/* protocol/thermal_wire_opcodes.h
 *
 * Stage 10 10a — wire-codec opcode + transport contract.
 *
 * Single source of truth for frame opcodes, decoder status codes,
 * version, header length, and per-platform receive caps.
 *
 * Command IDs live in `core/thermal_commands.h` per PRD §7.2 line
 * 941; this header only includes that header and adds compile-time
 * `_Static_assert`s pinning the numeric values that the wire path
 * encodes/decodes.  Adding a new command ID without updating the
 * assert here trips a hard build error at the wire layer.
 *
 * No `protocol/` code may depend on `platform/linux/` or `support/`.
 */
#ifndef THERMAL_PROTOCOL_WIRE_OPCODES_H
#define THERMAL_PROTOCOL_WIRE_OPCODES_H

#include <stdint.h>
#include "thermal_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* === Header constants (PRD §7.2 lines 902-911) ============= */

#define THERMAL_WIRE_MAGIC0      ((uint8_t)'T')
#define THERMAL_WIRE_MAGIC1      ((uint8_t)'C')
#define THERMAL_WIRE_VERSION     1u

/* magic(2) + version(1) + opcode(1) + seq(2) + payload_len(2) + ts_ms(4) */
#define THERMAL_WIRE_HEADER_LEN  12u

/* Trailing CRC-16 (when CRC is enabled). */
#define THERMAL_WIRE_CRC_LEN     2u

/* Per-platform default receive caps (PRD §7.2 line 923). */
#define THERMAL_WIRE_MAX_LINUX   1024u
#define THERMAL_WIRE_MAX_MCU     256u

/* === Opcodes (PRD §7.2 lines 925-933) ===================== */

typedef enum {
    THERMAL_WIRE_OP_TELEM_SAMPLE = 0x01,
    THERMAL_WIRE_OP_TELEM_EVENT  = 0x02,
    THERMAL_WIRE_OP_CMD_REQUEST  = 0x10,
    THERMAL_WIRE_OP_CMD_ACK      = 0x11,
    THERMAL_WIRE_OP_CMD_NACK     = 0x12
} thermal_wire_opcode_t;

/* === Decoder status (return values from encode/decode) ==== */
/* Local (negative) return codes from the C codec entry points; not on
 * the wire.  Distinct from the transport status namespace below. */

typedef enum {
    THERMAL_WIRE_OK                 =  0,
    THERMAL_WIRE_ERR_TRUNCATED      = -1,
    THERMAL_WIRE_ERR_BAD_MAGIC      = -2,
    THERMAL_WIRE_ERR_BAD_VERSION    = -3,
    THERMAL_WIRE_ERR_BAD_CRC        = -4,
    THERMAL_WIRE_ERR_BAD_OPCODE     = -5,
    THERMAL_WIRE_ERR_BAD_PAYLOAD    = -6,
    THERMAL_WIRE_ERR_OVER_CAP       = -7,
    THERMAL_WIRE_ERR_BUF_TOO_SMALL  = -8
} thermal_wire_status_t;

/* === Transport status codes (PRD §7.2 line 937) =========== */
/* Positive 16-bit values carried in the CMD_NACK `status` field for
 * transport-level rejects (frame parsed enough to extract a trusted
 * `seq` but failed downstream).  Allocated above 0x8000 so they
 * cannot collide with core `thermal_status_t` values, which live in
 * the low 16-bit range and use it for command-semantic rejects.
 *
 * Not in this namespace on purpose:
 *   - BAD_MAGIC / BAD_VERSION: header garbage; seq is not trustworthy,
 *     so the frame is silently dropped rather than NACKed.
 *   - BAD_CRC: the CRC check failed, so we cannot trust any field of
 *     the frame including seq.  Silent drop.
 *   - TRUNCATED (buf_sz < header): not enough bytes to extract a seq.
 *     Silent drop.
 *
 * Callers must therefore only emit a NACK with these codes when the
 * decode failure is BAD_OPCODE, OVER_CAP, or a per-opcode payload
 * decode failure (BAD_PAYLOAD) — cases where the outer header parsed
 * cleanly and `seq` was successfully recovered.
 */

typedef enum {
    THERMAL_WIRE_STATUS_OK            = 0x0000,
    THERMAL_WIRE_STATUS_BAD_PAYLOAD   = 0x8001,
    THERMAL_WIRE_STATUS_BAD_OPCODE    = 0x8002,
    THERMAL_WIRE_STATUS_OVER_CAP      = 0x8003,
    THERMAL_WIRE_STATUS_BAD_VERSION   = 0x8004
} thermal_wire_transport_status_t;

/* === Pinned command-ID values (PRD §7.2 line 941) ========= */
/* If any value in core/thermal_commands.h drifts, the wire path
 * would silently mis-encode.  These asserts make it a build break. */

_Static_assert(THERMAL_CMD_SET_PID         == 0x0001,
               "wire expects THERMAL_CMD_SET_PID = 0x0001");
_Static_assert(THERMAL_CMD_SET_SETPOINT    == 0x0002,
               "wire expects THERMAL_CMD_SET_SETPOINT = 0x0002");
_Static_assert(THERMAL_CMD_SET_TRIP        == 0x0003,
               "wire expects THERMAL_CMD_SET_TRIP = 0x0003");
_Static_assert(THERMAL_CMD_SET_CURVE_POINT == 0x0004,
               "wire expects THERMAL_CMD_SET_CURVE_POINT = 0x0004");
_Static_assert(THERMAL_CMD_CLEAR_FAULT     == 0x0005,
               "wire expects THERMAL_CMD_CLEAR_FAULT = 0x0005");

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_PROTOCOL_WIRE_OPCODES_H */
