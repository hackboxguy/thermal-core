/* protocol/thermal_wire.c
 *
 * Implementation of the canonical "TC" binary wire codec.
 * See thermal_wire.h for the contract.
 */
#include "thermal_wire.h"

#include <string.h>

/* =============================================================== */
/* Byte packers (little-endian; no memcpy over packed structs)     */
/* =============================================================== */

static void put_u8(uint8_t *p, uint8_t v)
{
    p[0] = v;
}

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)( v        & 0xFFu);
    p[1] = (uint8_t)((v >>  8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* =============================================================== */
/* CRC-16/CCITT-FALSE (PRD §7.2 lines 915-919)                     */
/* poly=0x1021, init=0xFFFF, refin=false, refout=false, xorout=0   */
/* Bit-by-bit; no table.  Canonical "123456789" -> 0x29B1.         */
/* =============================================================== */

uint16_t thermal_wire_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ ((uint16_t)buf[i] << 8));
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* =============================================================== */
/* Header writer / reader                                          */
/* =============================================================== */

/* Write the 12-byte header into `buf`.  Caller guarantees buf_sz
 * is at least THERMAL_WIRE_HEADER_LEN. */
static void write_header(uint8_t *buf,
                         thermal_wire_opcode_t opcode,
                         uint16_t seq, uint16_t payload_len,
                         uint32_t ts_ms)
{
    put_u8    (buf + 0, THERMAL_WIRE_MAGIC0);
    put_u8    (buf + 1, THERMAL_WIRE_MAGIC1);
    put_u8    (buf + 2, (uint8_t)THERMAL_WIRE_VERSION);
    put_u8    (buf + 3, (uint8_t)opcode);
    put_u16_le(buf + 4, seq);
    put_u16_le(buf + 6, payload_len);
    put_u32_le(buf + 8, ts_ms);
}

/* Finalise a frame: optionally append CRC.  Returns the total
 * byte count written (HEADER + payload + (crc_enabled ? 2 : 0)). */
static int finalize_frame(uint8_t *buf, size_t buf_sz,
                          size_t payload_len, int crc_enabled)
{
    size_t total = (size_t)THERMAL_WIRE_HEADER_LEN + payload_len;
    if (crc_enabled) total += THERMAL_WIRE_CRC_LEN;
    if (total > buf_sz) return THERMAL_WIRE_ERR_BUF_TOO_SMALL;
    if (crc_enabled) {
        uint16_t crc = thermal_wire_crc16(
            buf, (size_t)THERMAL_WIRE_HEADER_LEN + payload_len);
        put_u16_le(buf + THERMAL_WIRE_HEADER_LEN + payload_len, crc);
    }
    return (int)total;
}

/* Encode a complete frame given a payload writer that knows the
 * fixed payload size + how to fill it.  Returns the total written
 * byte count or negative status.  Inline helper used by all five
 * encode entry points. */
static int encode_frame(uint8_t *buf, size_t buf_sz,
                        thermal_wire_opcode_t opcode,
                        uint16_t seq, uint32_t ts_ms,
                        size_t payload_len,
                        int crc_enabled)
{
    if (payload_len > 0xFFFFu) {
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }
    size_t need = (size_t)THERMAL_WIRE_HEADER_LEN + payload_len;
    if (crc_enabled) need += THERMAL_WIRE_CRC_LEN;
    if (buf_sz < need) return THERMAL_WIRE_ERR_BUF_TOO_SMALL;

    write_header(buf, opcode, seq, (uint16_t)payload_len, ts_ms);
    return 0;     /* caller fills payload bytes, then calls finalize */
}

/* =============================================================== */
/* Encoders                                                         */
/* =============================================================== */

/* TELEM_SAMPLE payload: u16 signal_id, u16 flags, i32 value  = 8 bytes */
#define PL_TELEM_SAMPLE  8u

int thermal_wire_encode_telem_sample(uint8_t *buf, size_t buf_sz,
                                      uint16_t seq, uint32_t ts_ms,
                                      uint16_t signal_id,
                                      uint16_t flags,
                                      int32_t  value,
                                      int crc_enabled)
{
    int rc = encode_frame(buf, buf_sz, THERMAL_WIRE_OP_TELEM_SAMPLE,
                          seq, ts_ms, PL_TELEM_SAMPLE, crc_enabled);
    if (rc != 0) return rc;
    uint8_t *p = buf + THERMAL_WIRE_HEADER_LEN;
    put_u16_le(p + 0, signal_id);
    put_u16_le(p + 2, flags);
    put_u32_le(p + 4, (uint32_t)value);
    return finalize_frame(buf, buf_sz, PL_TELEM_SAMPLE, crc_enabled);
}

/* TELEM_EVENT payload: u16 event_code, u32 a1..a4 = 18 bytes */
#define PL_TELEM_EVENT  18u

int thermal_wire_encode_telem_event(uint8_t *buf, size_t buf_sz,
                                     uint16_t seq, uint32_t ts_ms,
                                     uint16_t event_code,
                                     uint32_t a1, uint32_t a2,
                                     uint32_t a3, uint32_t a4,
                                     int crc_enabled)
{
    int rc = encode_frame(buf, buf_sz, THERMAL_WIRE_OP_TELEM_EVENT,
                          seq, ts_ms, PL_TELEM_EVENT, crc_enabled);
    if (rc != 0) return rc;
    uint8_t *p = buf + THERMAL_WIRE_HEADER_LEN;
    put_u16_le(p +  0, event_code);
    put_u32_le(p +  2, a1);
    put_u32_le(p +  6, a2);
    put_u32_le(p + 10, a3);
    put_u32_le(p + 14, a4);
    return finalize_frame(buf, buf_sz, PL_TELEM_EVENT, crc_enabled);
}

/* CMD_REQUEST payload: u16 command_id + per-command struct.
 * Per-command sizes (PRD §7.5 lines 968-972):
 *   SET_PID         u16 zone, i32 kp, ki, kd                          = 14
 *   SET_SETPOINT    u16 zone, i32 setpoint_mc                         =  6
 *   SET_TRIP        u16 zone, u16 trip_idx, i32 temp, i32 hyst        = 12
 *   SET_CURVE_POINT u16 mod, u16 idx, i32 x, i32 v0, i32 v1           = 16
 *   CLEAR_FAULT     u16 fault_type, u16 target_id                     =  4
 */
#define PL_CMDID_HDR      2u
#define PL_SET_PID        (PL_CMDID_HDR + 14u)
#define PL_SET_SETPOINT   (PL_CMDID_HDR +  6u)
#define PL_SET_TRIP       (PL_CMDID_HDR + 12u)
#define PL_SET_CURVE      (PL_CMDID_HDR + 16u)
#define PL_CLEAR_FAULT    (PL_CMDID_HDR +  4u)

/* CMD_ACK / CMD_NACK payload: u16 request_seq, u16 status, u32 detail = 8 */
#define PL_CMD_ACKNACK    8u

static size_t cmd_payload_len(uint16_t command_id)
{
    switch (command_id) {
    case THERMAL_CMD_SET_PID:         return PL_SET_PID;
    case THERMAL_CMD_SET_SETPOINT:    return PL_SET_SETPOINT;
    case THERMAL_CMD_SET_TRIP:        return PL_SET_TRIP;
    case THERMAL_CMD_SET_CURVE_POINT: return PL_SET_CURVE;
    case THERMAL_CMD_CLEAR_FAULT:     return PL_CLEAR_FAULT;
    default:                          return 0;  /* signals "unknown" */
    }
}

int thermal_wire_encode_cmd_request(uint8_t *buf, size_t buf_sz,
                                     uint16_t seq, uint32_t ts_ms,
                                     const thermal_command_t *cmd,
                                     int crc_enabled)
{
    if (!cmd) return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    size_t pl = cmd_payload_len(cmd->command_id);
    if (pl == 0) return THERMAL_WIRE_ERR_BAD_PAYLOAD;

    int rc = encode_frame(buf, buf_sz, THERMAL_WIRE_OP_CMD_REQUEST,
                          seq, ts_ms, pl, crc_enabled);
    if (rc != 0) return rc;

    uint8_t *p = buf + THERMAL_WIRE_HEADER_LEN;
    put_u16_le(p, cmd->command_id);
    p += PL_CMDID_HDR;

    switch (cmd->command_id) {
    case THERMAL_CMD_SET_PID:
        put_u16_le(p +  0, cmd->u.set_pid.zone_id);
        put_u32_le(p +  2, (uint32_t)cmd->u.set_pid.kp_q16);
        put_u32_le(p +  6, (uint32_t)cmd->u.set_pid.ki_q16);
        put_u32_le(p + 10, (uint32_t)cmd->u.set_pid.kd_q16);
        break;
    case THERMAL_CMD_SET_SETPOINT:
        put_u16_le(p + 0, cmd->u.set_setpoint.zone_id);
        put_u32_le(p + 2, (uint32_t)cmd->u.set_setpoint.setpoint_mc);
        break;
    case THERMAL_CMD_SET_TRIP:
        put_u16_le(p + 0, cmd->u.set_trip.zone_id);
        put_u16_le(p + 2, cmd->u.set_trip.trip_idx);
        put_u32_le(p + 4, (uint32_t)cmd->u.set_trip.temp_mc);
        put_u32_le(p + 8, (uint32_t)cmd->u.set_trip.hyst_mc);
        break;
    case THERMAL_CMD_SET_CURVE_POINT:
        put_u16_le(p +  0, cmd->u.set_curve_point.modifier_id);
        put_u16_le(p +  2, cmd->u.set_curve_point.point_idx);
        put_u32_le(p +  4, (uint32_t)cmd->u.set_curve_point.x);
        put_u32_le(p +  8, (uint32_t)cmd->u.set_curve_point.value0);
        put_u32_le(p + 12, (uint32_t)cmd->u.set_curve_point.value1);
        break;
    case THERMAL_CMD_CLEAR_FAULT:
        put_u16_le(p + 0, cmd->u.clear_fault.fault_type);
        put_u16_le(p + 2, cmd->u.clear_fault.target_id);
        break;
    default:
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }

    return finalize_frame(buf, buf_sz, pl, crc_enabled);
}

static int encode_cmd_acknack(uint8_t *buf, size_t buf_sz,
                              thermal_wire_opcode_t opcode,
                              uint16_t seq, uint32_t ts_ms,
                              uint16_t request_seq,
                              uint16_t status,
                              uint32_t detail_code,
                              int crc_enabled)
{
    int rc = encode_frame(buf, buf_sz, opcode, seq, ts_ms,
                          PL_CMD_ACKNACK, crc_enabled);
    if (rc != 0) return rc;
    uint8_t *p = buf + THERMAL_WIRE_HEADER_LEN;
    put_u16_le(p + 0, request_seq);
    put_u16_le(p + 2, status);
    put_u32_le(p + 4, detail_code);
    return finalize_frame(buf, buf_sz, PL_CMD_ACKNACK, crc_enabled);
}

int thermal_wire_encode_cmd_ack(uint8_t *buf, size_t buf_sz,
                                 uint16_t seq, uint32_t ts_ms,
                                 uint16_t request_seq,
                                 uint16_t status,
                                 uint32_t detail_code,
                                 int crc_enabled)
{
    return encode_cmd_acknack(buf, buf_sz, THERMAL_WIRE_OP_CMD_ACK,
                              seq, ts_ms, request_seq, status,
                              detail_code, crc_enabled);
}

int thermal_wire_encode_cmd_nack(uint8_t *buf, size_t buf_sz,
                                  uint16_t seq, uint32_t ts_ms,
                                  uint16_t request_seq,
                                  uint16_t status,
                                  uint32_t detail_code,
                                  int crc_enabled)
{
    return encode_cmd_acknack(buf, buf_sz, THERMAL_WIRE_OP_CMD_NACK,
                              seq, ts_ms, request_seq, status,
                              detail_code, crc_enabled);
}

/* =============================================================== */
/* Decoders                                                         */
/* =============================================================== */

static int opcode_is_known(uint8_t v)
{
    switch (v) {
    case THERMAL_WIRE_OP_TELEM_SAMPLE:
    case THERMAL_WIRE_OP_TELEM_EVENT:
    case THERMAL_WIRE_OP_CMD_REQUEST:
    case THERMAL_WIRE_OP_CMD_ACK:
    case THERMAL_WIRE_OP_CMD_NACK:
        return 1;
    default:
        return 0;
    }
}

int thermal_wire_decode_frame(const uint8_t *buf, size_t buf_sz,
                               size_t receive_cap,
                               int crc_enabled,
                               thermal_wire_frame_t *out)
{
    if (!buf || !out) return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    if (buf_sz < THERMAL_WIRE_HEADER_LEN) return THERMAL_WIRE_ERR_TRUNCATED;
    if (buf[0] != THERMAL_WIRE_MAGIC0 || buf[1] != THERMAL_WIRE_MAGIC1) {
        return THERMAL_WIRE_ERR_BAD_MAGIC;
    }
    if (buf[2] != THERMAL_WIRE_VERSION) {
        return THERMAL_WIRE_ERR_BAD_VERSION;
    }
    uint8_t opcode_raw = buf[3];
    if (!opcode_is_known(opcode_raw)) {
        return THERMAL_WIRE_ERR_BAD_OPCODE;
    }
    uint16_t seq         = get_u16_le(buf + 4);
    uint16_t payload_len = get_u16_le(buf + 6);
    uint32_t ts_ms       = get_u32_le(buf + 8);

    if (receive_cap > 0 && payload_len > receive_cap) {
        return THERMAL_WIRE_ERR_OVER_CAP;
    }

    size_t need = (size_t)THERMAL_WIRE_HEADER_LEN + payload_len;
    if (crc_enabled) need += THERMAL_WIRE_CRC_LEN;
    if (buf_sz < need) return THERMAL_WIRE_ERR_TRUNCATED;

    if (crc_enabled) {
        uint16_t got      = get_u16_le(buf + THERMAL_WIRE_HEADER_LEN + payload_len);
        uint16_t expected = thermal_wire_crc16(
            buf, (size_t)THERMAL_WIRE_HEADER_LEN + payload_len);
        if (got != expected) {
            return THERMAL_WIRE_ERR_BAD_CRC;
        }
    }

    out->seq         = seq;
    out->ts_ms       = ts_ms;
    out->opcode      = (thermal_wire_opcode_t)opcode_raw;
    out->payload     = buf + THERMAL_WIRE_HEADER_LEN;
    out->payload_len = payload_len;
    out->crc_enabled = crc_enabled;
    return THERMAL_WIRE_OK;
}

/* === Per-opcode payload decoders ================================ */

int thermal_wire_decode_telem_sample(const thermal_wire_frame_t *frame,
                                      uint16_t *signal_id,
                                      uint16_t *flags,
                                      int32_t  *value)
{
    if (!frame || !signal_id || !flags || !value) {
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }
    if (frame->opcode != THERMAL_WIRE_OP_TELEM_SAMPLE ||
        frame->payload_len != PL_TELEM_SAMPLE) {
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }
    const uint8_t *p = frame->payload;
    *signal_id = get_u16_le(p + 0);
    *flags     = get_u16_le(p + 2);
    *value     = (int32_t)get_u32_le(p + 4);
    return THERMAL_WIRE_OK;
}

int thermal_wire_decode_telem_event(const thermal_wire_frame_t *frame,
                                     uint16_t *event_code,
                                     uint32_t *a1, uint32_t *a2,
                                     uint32_t *a3, uint32_t *a4)
{
    if (!frame || !event_code || !a1 || !a2 || !a3 || !a4) {
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }
    if (frame->opcode != THERMAL_WIRE_OP_TELEM_EVENT ||
        frame->payload_len != PL_TELEM_EVENT) {
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }
    const uint8_t *p = frame->payload;
    *event_code = get_u16_le(p +  0);
    *a1         = get_u32_le(p +  2);
    *a2         = get_u32_le(p +  6);
    *a3         = get_u32_le(p + 10);
    *a4         = get_u32_le(p + 14);
    return THERMAL_WIRE_OK;
}

int thermal_wire_decode_cmd_request(const thermal_wire_frame_t *frame,
                                     thermal_command_t *cmd)
{
    if (!frame || !cmd) return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    if (frame->opcode != THERMAL_WIRE_OP_CMD_REQUEST) {
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }
    if (frame->payload_len < PL_CMDID_HDR) {
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }
    const uint8_t *p = frame->payload;
    uint16_t command_id = get_u16_le(p);
    size_t expected = cmd_payload_len(command_id);
    if (expected == 0 || expected != frame->payload_len) {
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->command_id = command_id;
    p += PL_CMDID_HDR;

    switch (command_id) {
    case THERMAL_CMD_SET_PID:
        cmd->u.set_pid.zone_id = get_u16_le(p + 0);
        cmd->u.set_pid.kp_q16  = (int32_t)get_u32_le(p + 2);
        cmd->u.set_pid.ki_q16  = (int32_t)get_u32_le(p + 6);
        cmd->u.set_pid.kd_q16  = (int32_t)get_u32_le(p + 10);
        break;
    case THERMAL_CMD_SET_SETPOINT:
        cmd->u.set_setpoint.zone_id     = get_u16_le(p + 0);
        cmd->u.set_setpoint.setpoint_mc = (int32_t)get_u32_le(p + 2);
        break;
    case THERMAL_CMD_SET_TRIP:
        cmd->u.set_trip.zone_id  = get_u16_le(p + 0);
        cmd->u.set_trip.trip_idx = get_u16_le(p + 2);
        cmd->u.set_trip.temp_mc  = (int32_t)get_u32_le(p + 4);
        cmd->u.set_trip.hyst_mc  = (int32_t)get_u32_le(p + 8);
        break;
    case THERMAL_CMD_SET_CURVE_POINT:
        cmd->u.set_curve_point.modifier_id = get_u16_le(p +  0);
        cmd->u.set_curve_point.point_idx   = get_u16_le(p +  2);
        cmd->u.set_curve_point.x           = (int32_t)get_u32_le(p +  4);
        cmd->u.set_curve_point.value0      = (int32_t)get_u32_le(p +  8);
        cmd->u.set_curve_point.value1      = (int32_t)get_u32_le(p + 12);
        break;
    case THERMAL_CMD_CLEAR_FAULT:
        cmd->u.clear_fault.fault_type = get_u16_le(p + 0);
        cmd->u.clear_fault.target_id  = get_u16_le(p + 2);
        break;
    default:
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }
    return THERMAL_WIRE_OK;
}

int thermal_wire_decode_cmd_ack_or_nack(const thermal_wire_frame_t *frame,
                                         uint16_t *request_seq,
                                         uint16_t *status,
                                         uint32_t *detail_code)
{
    if (!frame || !request_seq || !status || !detail_code) {
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }
    if ((frame->opcode != THERMAL_WIRE_OP_CMD_ACK &&
         frame->opcode != THERMAL_WIRE_OP_CMD_NACK) ||
        frame->payload_len != PL_CMD_ACKNACK) {
        return THERMAL_WIRE_ERR_BAD_PAYLOAD;
    }
    const uint8_t *p = frame->payload;
    *request_seq = get_u16_le(p + 0);
    *status      = get_u16_le(p + 2);
    *detail_code = get_u32_le(p + 4);
    return THERMAL_WIRE_OK;
}
