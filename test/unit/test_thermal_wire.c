/* test/unit/test_thermal_wire.c
 *
 * Stage 10 10a — protocol/thermal_wire.{c,h} unit tests.
 *
 * Scenarios (one TEST_CASE):
 *   1. CRC-16/CCITT-FALSE vector ("123456789" -> 0x29B1).
 *   2. TELEM_SAMPLE round-trip w/ CRC.
 *   3. TELEM_SAMPLE round-trip w/o CRC.
 *   4. TELEM_EVENT round-trip.
 *   5-9. CMD_REQUEST round-trip for all five command IDs.
 *   10. CMD_ACK round-trip.
 *   11. CMD_NACK round-trip.
 *   12. Bad magic -> ERR_BAD_MAGIC.
 *   13. Bad version -> ERR_BAD_VERSION.
 *   14. Bad CRC -> ERR_BAD_CRC.
 *   15. Reserved opcode -> ERR_BAD_OPCODE.
 *   16. Over-cap -> ERR_OVER_CAP.
 *   17. Buffer too small for encode -> ERR_BUF_TOO_SMALL.
 *
 * No socket I/O; everything runs against in-memory byte buffers.
 */
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "thermal_commands.h"
#include "thermal_wire.h"
#include "thermal_wire_opcodes.h"

TEST_CASE(thermal_wire) {
    /* === Scenario 1: canonical CRC vector ============================ */
    {
        const uint8_t v[] = "123456789";
        EXPECT_EQ(thermal_wire_crc16(v, 9), 0x29B1);
    }

    /* === Scenario 2: TELEM_SAMPLE round-trip w/ CRC =================== */
    {
        uint8_t buf[64];
        int n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                                 0x1234, 0xABCD1234,
                                                 0x0100, 0x0001, -7,
                                                 /*crc*/ 1);
        EXPECT_EQ(n, (int)(THERMAL_WIRE_HEADER_LEN + 8 + THERMAL_WIRE_CRC_LEN));
        EXPECT_EQ(buf[0], 'T'); EXPECT_EQ(buf[1], 'C');
        EXPECT_EQ(buf[2], 1);
        EXPECT_EQ(buf[3], THERMAL_WIRE_OP_TELEM_SAMPLE);

        thermal_wire_frame_t fr;
        int rc = thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX,
                                            /*crc*/ 1, &fr);
        EXPECT_EQ(rc, THERMAL_WIRE_OK);
        EXPECT_EQ(fr.seq,         0x1234);
        EXPECT_EQ(fr.ts_ms,       0xABCD1234);
        EXPECT_EQ(fr.opcode,      THERMAL_WIRE_OP_TELEM_SAMPLE);
        EXPECT_EQ(fr.payload_len, 8u);

        uint16_t sig, flags; int32_t val;
        EXPECT_EQ(thermal_wire_decode_telem_sample(&fr, &sig, &flags, &val),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(sig,   0x0100);
        EXPECT_EQ(flags, 0x0001);
        EXPECT_EQ(val,   -7);
    }

    /* === Scenario 3: TELEM_SAMPLE w/o CRC ============================ */
    {
        uint8_t buf[64];
        int n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                                 1, 100, 0x0200, 0, 80,
                                                 /*crc*/ 0);
        EXPECT_EQ(n, (int)(THERMAL_WIRE_HEADER_LEN + 8));
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 0, &fr),
                  THERMAL_WIRE_OK);
        uint16_t sig, flags; int32_t val;
        EXPECT_EQ(thermal_wire_decode_telem_sample(&fr, &sig, &flags, &val),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(val, 80);
    }

    /* === Scenario 4: TELEM_EVENT round-trip ========================== */
    {
        uint8_t buf[64];
        int n = thermal_wire_encode_telem_event(buf, sizeof(buf),
                                                 42, 5000, 0x1000,
                                                 0xAABBCCDD, 2, 3, 4,
                                                 /*crc*/ 1);
        EXPECT_EQ(n, (int)(THERMAL_WIRE_HEADER_LEN + 18 + THERMAL_WIRE_CRC_LEN));

        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_OK);
        uint16_t code; uint32_t a1, a2, a3, a4;
        EXPECT_EQ(thermal_wire_decode_telem_event(&fr, &code, &a1, &a2, &a3, &a4),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(code, 0x1000);
        EXPECT_EQ(a1,   0xAABBCCDD);
        EXPECT_EQ(a4,   4);
    }

    /* === Scenarios 5-9: CMD_REQUEST round-trips ====================== */
    /* SET_PID */
    {
        thermal_command_t cmd_in, cmd_out;
        memset(&cmd_in, 0, sizeof(cmd_in));
        cmd_in.command_id            = THERMAL_CMD_SET_PID;
        cmd_in.u.set_pid.zone_id     = 0;
        cmd_in.u.set_pid.kp_q16      = 5000;
        cmd_in.u.set_pid.ki_q16      = 400;
        cmd_in.u.set_pid.kd_q16      = 0;
        uint8_t buf[64];
        int n = thermal_wire_encode_cmd_request(buf, sizeof(buf), 1, 100,
                                                &cmd_in, 1);
        EXPECT_LE(n, (int)sizeof(buf));
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(fr.opcode, THERMAL_WIRE_OP_CMD_REQUEST);
        EXPECT_EQ(thermal_wire_decode_cmd_request(&fr, &cmd_out),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(cmd_out.command_id,        THERMAL_CMD_SET_PID);
        EXPECT_EQ(cmd_out.u.set_pid.zone_id, 0);
        EXPECT_EQ(cmd_out.u.set_pid.kp_q16,  5000);
        EXPECT_EQ(cmd_out.u.set_pid.ki_q16,  400);
        EXPECT_EQ(cmd_out.u.set_pid.kd_q16,  0);
    }
    /* SET_SETPOINT */
    {
        thermal_command_t cmd_in, cmd_out;
        memset(&cmd_in, 0, sizeof(cmd_in));
        cmd_in.command_id               = THERMAL_CMD_SET_SETPOINT;
        cmd_in.u.set_setpoint.zone_id   = 1;
        cmd_in.u.set_setpoint.setpoint_mc = 75000;
        uint8_t buf[64];
        int n = thermal_wire_encode_cmd_request(buf, sizeof(buf), 2, 200,
                                                &cmd_in, 1);
        EXPECT_LE(n, (int)sizeof(buf));
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(thermal_wire_decode_cmd_request(&fr, &cmd_out),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(cmd_out.u.set_setpoint.zone_id,     1);
        EXPECT_EQ(cmd_out.u.set_setpoint.setpoint_mc, 75000);
    }
    /* SET_TRIP */
    {
        thermal_command_t cmd_in, cmd_out;
        memset(&cmd_in, 0, sizeof(cmd_in));
        cmd_in.command_id           = THERMAL_CMD_SET_TRIP;
        cmd_in.u.set_trip.zone_id   = 0;
        cmd_in.u.set_trip.trip_idx  = 1;
        cmd_in.u.set_trip.temp_mc   = 95000;
        cmd_in.u.set_trip.hyst_mc   = 3000;
        uint8_t buf[64];
        int n = thermal_wire_encode_cmd_request(buf, sizeof(buf), 3, 300,
                                                &cmd_in, 1);
        EXPECT_LE(n, (int)sizeof(buf));
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(thermal_wire_decode_cmd_request(&fr, &cmd_out),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(cmd_out.u.set_trip.trip_idx, 1);
        EXPECT_EQ(cmd_out.u.set_trip.temp_mc,  95000);
        EXPECT_EQ(cmd_out.u.set_trip.hyst_mc,  3000);
    }
    /* SET_CURVE_POINT */
    {
        thermal_command_t cmd_in, cmd_out;
        memset(&cmd_in, 0, sizeof(cmd_in));
        cmd_in.command_id                       = THERMAL_CMD_SET_CURVE_POINT;
        cmd_in.u.set_curve_point.modifier_id    = 0;
        cmd_in.u.set_curve_point.point_idx      = 1;
        cmd_in.u.set_curve_point.x              = 30000;
        cmd_in.u.set_curve_point.value0         = 255;
        cmd_in.u.set_curve_point.value1         = 0;
        uint8_t buf[64];
        int n = thermal_wire_encode_cmd_request(buf, sizeof(buf), 4, 400,
                                                &cmd_in, 1);
        EXPECT_LE(n, (int)sizeof(buf));
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(thermal_wire_decode_cmd_request(&fr, &cmd_out),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(cmd_out.u.set_curve_point.modifier_id, 0);
        EXPECT_EQ(cmd_out.u.set_curve_point.point_idx,   1);
        EXPECT_EQ(cmd_out.u.set_curve_point.x,           30000);
        EXPECT_EQ(cmd_out.u.set_curve_point.value0,      255);
        EXPECT_EQ(cmd_out.u.set_curve_point.value1,      0);
    }
    /* CLEAR_FAULT */
    {
        thermal_command_t cmd_in, cmd_out;
        memset(&cmd_in, 0, sizeof(cmd_in));
        cmd_in.command_id              = THERMAL_CMD_CLEAR_FAULT;
        cmd_in.u.clear_fault.fault_type = 1;  /* STALL */
        cmd_in.u.clear_fault.target_id  = 0;
        uint8_t buf[64];
        int n = thermal_wire_encode_cmd_request(buf, sizeof(buf), 5, 500,
                                                &cmd_in, 1);
        EXPECT_LE(n, (int)sizeof(buf));
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(thermal_wire_decode_cmd_request(&fr, &cmd_out),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(cmd_out.u.clear_fault.fault_type, 1);
        EXPECT_EQ(cmd_out.u.clear_fault.target_id,  0);
    }

    /* === Scenario 10: CMD_ACK round-trip ============================ */
    {
        uint8_t buf[64];
        int n = thermal_wire_encode_cmd_ack(buf, sizeof(buf), 10, 1000,
                                            /*request_seq=*/ 5,
                                            /*status=*/ 0,
                                            /*detail=*/ 0xCAFEBABE,
                                            1);
        EXPECT_LE(n, (int)sizeof(buf));
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(fr.opcode, THERMAL_WIRE_OP_CMD_ACK);
        uint16_t req, st; uint32_t det;
        EXPECT_EQ(thermal_wire_decode_cmd_ack_or_nack(&fr, &req, &st, &det),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(req, 5);
        EXPECT_EQ(st,  0);
        EXPECT_EQ(det, 0xCAFEBABE);
    }

    /* === Scenario 11: CMD_NACK round-trip =========================== */
    {
        uint8_t buf[64];
        int n = thermal_wire_encode_cmd_nack(buf, sizeof(buf), 11, 1100,
                                             5, 1 /*INVALID_ARG*/, 0,
                                             1);
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(fr.opcode, THERMAL_WIRE_OP_CMD_NACK);
        uint16_t req, st; uint32_t det;
        EXPECT_EQ(thermal_wire_decode_cmd_ack_or_nack(&fr, &req, &st, &det),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(req, 5);
        EXPECT_EQ(st,  1);
    }

    /* === Scenario 12: bad magic ===================================== */
    {
        uint8_t buf[64];
        int n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                                 1, 0, 0, 0, 0, 1);
        buf[0] = 'X';
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_ERR_BAD_MAGIC);
    }

    /* === Scenario 13: bad version =================================== */
    {
        uint8_t buf[64];
        int n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                                 1, 0, 0, 0, 0, 1);
        buf[2] = 99;
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_ERR_BAD_VERSION);
    }

    /* === Scenario 14: bad CRC ====================================== */
    {
        uint8_t buf[64];
        int n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                                 1, 0, 0, 0, 0, 1);
        /* Flip a payload byte; CRC no longer matches. */
        buf[THERMAL_WIRE_HEADER_LEN + 2] ^= 0xFFu;
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_ERR_BAD_CRC);
    }

    /* === Scenario 15: reserved opcode =============================== */
    {
        uint8_t buf[64];
        int n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                                 1, 0, 0, 0, 0, 0);
        buf[3] = 0x05;   /* reserved per PRD §7.2 line 935 */
        thermal_wire_frame_t fr;
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 0, &fr),
                  THERMAL_WIRE_ERR_BAD_OPCODE);
    }

    /* === Scenario 16: over-cap ===================================== */
    {
        uint8_t buf[64];
        int n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                                 1, 0, 0, 0, 0, 0);
        thermal_wire_frame_t fr;
        /* Pass a receive_cap smaller than the actual payload_len = 8. */
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            /*cap*/ 4, 0, &fr),
                  THERMAL_WIRE_ERR_OVER_CAP);
    }

    /* === Scenario 17: buffer too small for encode =================== */
    {
        uint8_t buf[4];
        int n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                                 1, 0, 0, 0, 0, 0);
        EXPECT_EQ(n, THERMAL_WIRE_ERR_BUF_TOO_SMALL);
    }

    /* === Scenario 18: u16 seq boundary (wraparound) =============== */
    /* PRD section 7.2 line 937: seq wraps modulo 65536.  This
     * scenario covers the wire-codec boundary: encode/decode at
     * seq=65535 and seq=0 (post-wrap) must both round-trip cleanly.
     * The host-side outstanding-window matching test (impl-plan
     * section 5 Stage 10) requires an async client and lands when
     * the Stage 12 scenario runner ships. */
    {
        uint8_t buf[64];
        thermal_wire_frame_t fr;

        int n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                                 0xFFFFu, 0, 0, 0, 0, 1);
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(fr.seq, 0xFFFFu);

        n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                             0u, 0, 0, 0, 0, 1);
        EXPECT_EQ(thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX, 1, &fr),
                  THERMAL_WIRE_OK);
        EXPECT_EQ(fr.seq, 0u);
    }
}
