/* test/unit/test_obd2.c
 *
 * Stage 11 11a — protocol/obd2.{c,h} unit tests.
 *
 * Scenarios (one TEST_CASE):
 *   1. Encode request, PID = 0x0D -> exact byte pattern.
 *   2. Encode request, PID = 0x0C -> byte 2 changes, rest identical.
 *   3. Decode well-formed response, value = 120 km/h.
 *   4. Decode response with value = 0.
 *   5. Decode response with value = 255.
 *   6. Wrong service byte -> ERR_WRONG_SERVICE.
 *   7. Wrong PID echo -> ERR_WRONG_PID.
 *   8. Negative response (0x7F 0x01 0x12) -> ERR_NEGATIVE_RESPONSE.
 *   9. Short input (in_len = 4) -> ERR_BAD_SHAPE.
 *   10. Multi-frame PCI byte (0x10) -> ERR_BAD_SHAPE.
 *   11. Single-frame PCI length disagrees (PCI=0x04) -> ERR_WRONG_PCI_LEN.
 *
 * No socket I/O; everything runs against in-memory byte buffers.
 */
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "obd2.h"

TEST_CASE(obd2) {
    /* === Scenario 1: encode request, PID = 0x0D ===================== */
    {
        uint8_t buf[OBD2_FRAME_DLC] = {0};
        obd2_encode_request_byte(OBD2_PID_VEHICLE_SPEED, buf);
        EXPECT_EQ(buf[0], 0x02);
        EXPECT_EQ(buf[1], OBD2_SERVICE_01_CURRENT_DATA);
        EXPECT_EQ(buf[2], OBD2_PID_VEHICLE_SPEED);
        EXPECT_EQ(buf[3], 0x55);
        EXPECT_EQ(buf[4], 0x55);
        EXPECT_EQ(buf[5], 0x55);
        EXPECT_EQ(buf[6], 0x55);
        EXPECT_EQ(buf[7], 0x55);
    }

    /* === Scenario 2: encode request, PID = 0x0C (rpm) =============== */
    /* The encoder is generic over single-byte Service 01 PIDs.  Only
     * byte 2 should differ from scenario 1. */
    {
        uint8_t buf[OBD2_FRAME_DLC] = {0};
        obd2_encode_request_byte(0x0Cu, buf);
        EXPECT_EQ(buf[0], 0x02);
        EXPECT_EQ(buf[1], 0x01);
        EXPECT_EQ(buf[2], 0x0C);
        EXPECT_EQ(buf[3], 0x55);
        EXPECT_EQ(buf[7], 0x55);
    }

    /* === Scenario 3: decode response, value = 120 km/h ============== */
    {
        const uint8_t resp[OBD2_FRAME_DLC] = {
            0x03, 0x41, OBD2_PID_VEHICLE_SPEED, 120,
            0x55, 0x55, 0x55, 0x55
        };
        uint8_t value = 0xAA;
        int rc = obd2_decode_response_byte(resp, sizeof(resp),
                                            OBD2_PID_VEHICLE_SPEED, &value);
        EXPECT_EQ(rc, OBD2_OK);
        EXPECT_EQ(value, 120);
    }

    /* === Scenario 4: decode response, value = 0 (vehicle stopped) === */
    {
        const uint8_t resp[OBD2_FRAME_DLC] = {
            0x03, 0x41, OBD2_PID_VEHICLE_SPEED, 0,
            0x55, 0x55, 0x55, 0x55
        };
        uint8_t value = 0xAA;
        int rc = obd2_decode_response_byte(resp, sizeof(resp),
                                            OBD2_PID_VEHICLE_SPEED, &value);
        EXPECT_EQ(rc, OBD2_OK);
        EXPECT_EQ(value, 0);
    }

    /* === Scenario 5: decode response, value = 255 (max km/h) ======== */
    /* PRD §6.1 line 814: OBD-II speed range 0..255 km/h.  Make sure
     * the high end doesn't get sign-flipped. */
    {
        const uint8_t resp[OBD2_FRAME_DLC] = {
            0x03, 0x41, OBD2_PID_VEHICLE_SPEED, 255,
            0x55, 0x55, 0x55, 0x55
        };
        uint8_t value = 0;
        int rc = obd2_decode_response_byte(resp, sizeof(resp),
                                            OBD2_PID_VEHICLE_SPEED, &value);
        EXPECT_EQ(rc, OBD2_OK);
        EXPECT_EQ(value, 255);
    }

    /* === Scenario 6: wrong service byte ============================ */
    /* Response service 0x42 != 0x41 (Service 02 response).  Probably
     * a frame from a different request that we don't care about. */
    {
        const uint8_t resp[OBD2_FRAME_DLC] = {
            0x03, 0x42, OBD2_PID_VEHICLE_SPEED, 120,
            0x55, 0x55, 0x55, 0x55
        };
        uint8_t value = 0xAA;
        int rc = obd2_decode_response_byte(resp, sizeof(resp),
                                            OBD2_PID_VEHICLE_SPEED, &value);
        EXPECT_EQ(rc, OBD2_ERR_WRONG_SERVICE);
        EXPECT_EQ(value, 0xAA);   /* unchanged on failure */
    }

    /* === Scenario 7: wrong PID echo ================================ */
    /* Asked for speed (0x0D); response echoes rpm (0x0C). */
    {
        const uint8_t resp[OBD2_FRAME_DLC] = {
            0x03, 0x41, 0x0C, 120,
            0x55, 0x55, 0x55, 0x55
        };
        uint8_t value = 0xAA;
        int rc = obd2_decode_response_byte(resp, sizeof(resp),
                                            OBD2_PID_VEHICLE_SPEED, &value);
        EXPECT_EQ(rc, OBD2_ERR_WRONG_PID);
    }

    /* === Scenario 8: negative response ============================== */
    /* 0x7F = negative response; 0x01 = rejected service; 0x12 = NRC
     * subFunctionNotSupported.  Must be distinguished from
     * WRONG_SERVICE so the BSP can log it as a real ECU answer. */
    {
        const uint8_t resp[OBD2_FRAME_DLC] = {
            0x03, OBD2_NEGATIVE_RESPONSE, OBD2_SERVICE_01_CURRENT_DATA, 0x12,
            0x55, 0x55, 0x55, 0x55
        };
        uint8_t value = 0xAA;
        int rc = obd2_decode_response_byte(resp, sizeof(resp),
                                            OBD2_PID_VEHICLE_SPEED, &value);
        EXPECT_EQ(rc, OBD2_ERR_NEGATIVE_RESPONSE);
    }

    /* === Scenario 9: short input =================================== */
    /* SocketCAN gives DLC = 8 always (CAN classic), but a misbehaving
     * transport (or a CAN-FD frame slipped through) might deliver
     * fewer bytes.  Reject so we never read past the buffer. */
    {
        const uint8_t resp[4] = { 0x03, 0x41, 0x0D, 120 };
        uint8_t value = 0xAA;
        int rc = obd2_decode_response_byte(resp, sizeof(resp),
                                            OBD2_PID_VEHICLE_SPEED, &value);
        EXPECT_EQ(rc, OBD2_ERR_BAD_SHAPE);
    }

    /* === Scenario 10: multi-frame PCI byte ========================= */
    /* PCI high nibble 0x1 = first-frame, 0x2 = consecutive-frame,
     * 0x3 = flow-control.  v1 ISO-TP only accepts single-frame
     * (high nibble 0); anything else requires reassembly we don't
     * implement.  Pin the rule so 11b's BSP can't trip over it. */
    {
        const uint8_t resp[OBD2_FRAME_DLC] = {
            0x10, 0x06, 0x41, OBD2_PID_VEHICLE_SPEED,
            120, 0x55, 0x55, 0x55
        };
        uint8_t value = 0xAA;
        int rc = obd2_decode_response_byte(resp, sizeof(resp),
                                            OBD2_PID_VEHICLE_SPEED, &value);
        EXPECT_EQ(rc, OBD2_ERR_BAD_SHAPE);
    }

    /* === Scenario 11: PCI length disagrees with shape ============== */
    /* Single-frame PCI (high nibble 0) but the length field says 4
     * bytes of payload, not 3.  v1 only knows single-byte responses
     * (length 3); 4 implies a 2-byte value we are not configured
     * for.  Reject rather than silently read the wrong byte. */
    {
        const uint8_t resp[OBD2_FRAME_DLC] = {
            0x04, 0x41, OBD2_PID_VEHICLE_SPEED, 120,
            42, 0x55, 0x55, 0x55
        };
        uint8_t value = 0xAA;
        int rc = obd2_decode_response_byte(resp, sizeof(resp),
                                            OBD2_PID_VEHICLE_SPEED, &value);
        EXPECT_EQ(rc, OBD2_ERR_WRONG_PCI_LEN);
    }
}
