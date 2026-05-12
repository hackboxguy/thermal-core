/* test/unit/test_telem_wire.c
 *
 * Unit tests for the v1 TELEM_SAMPLE / TELEM_EVENT wire encoder.
 * Pure byte-layout checks; no socket I/O.
 *
 * Scenarios:
 *   1. pack_sample byte layout (type, ts_ms, signal_id, value).
 *   2. pack_event byte layout (type, ts_ms, code, a1..a4).
 *   3. Frame-length constants match the documented sizes.
 *   4. Endianness: pack value 0x01020304, expect bytes 04 03 02 01.
 *   5. Negative value round-trip (int32 stored as little-endian
 *      twos-complement bytes).
 */
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "telem_wire.h"

TEST_CASE(telem_wire) {
    /* === Scenario 1: pack_sample byte layout =========================== */
    {
        uint8_t buf[TELEM_WIRE_SAMPLE_LEN];
        size_t n = telem_wire_pack_sample(buf, 0x12345678u, 0x0100u, 75000);
        EXPECT_EQ(n, TELEM_WIRE_SAMPLE_LEN);
        /* type = 0x01 */
        EXPECT_EQ(buf[0], 0x01);
        /* ts_ms = 0x12345678 -> 78 56 34 12 little-endian */
        EXPECT_EQ(buf[1], 0x78);
        EXPECT_EQ(buf[2], 0x56);
        EXPECT_EQ(buf[3], 0x34);
        EXPECT_EQ(buf[4], 0x12);
        /* signal_id = 0x0100 -> 00 01 */
        EXPECT_EQ(buf[5], 0x00);
        EXPECT_EQ(buf[6], 0x01);
        /* value = 75000 = 0x000124F8 -> F8 24 01 00 */
        EXPECT_EQ(buf[7], 0xF8);
        EXPECT_EQ(buf[8], 0x24);
        EXPECT_EQ(buf[9], 0x01);
        EXPECT_EQ(buf[10], 0x00);
    }

    /* === Scenario 2: pack_event byte layout ============================ */
    {
        uint8_t buf[TELEM_WIRE_EVENT_LEN];
        size_t n = telem_wire_pack_event(buf,
                                         0xAABBCCDDu,
                                         0x1000u,
                                         1, 2, 3, 4);
        EXPECT_EQ(n, TELEM_WIRE_EVENT_LEN);
        EXPECT_EQ(buf[0], 0x02);
        /* ts_ms = 0xAABBCCDD -> DD CC BB AA */
        EXPECT_EQ(buf[1], 0xDD);
        EXPECT_EQ(buf[2], 0xCC);
        EXPECT_EQ(buf[3], 0xBB);
        EXPECT_EQ(buf[4], 0xAA);
        /* code = 0x1000 -> 00 10 */
        EXPECT_EQ(buf[5], 0x00);
        EXPECT_EQ(buf[6], 0x10);
        /* a1 = 1 -> 01 00 00 00 */
        EXPECT_EQ(buf[7],  0x01); EXPECT_EQ(buf[8],  0x00);
        EXPECT_EQ(buf[9],  0x00); EXPECT_EQ(buf[10], 0x00);
        /* a2 = 2 -> 02 00 00 00 */
        EXPECT_EQ(buf[11], 0x02); EXPECT_EQ(buf[14], 0x00);
        /* a3 = 3 -> 03 ... */
        EXPECT_EQ(buf[15], 0x03);
        /* a4 = 4 -> 04 ... */
        EXPECT_EQ(buf[19], 0x04); EXPECT_EQ(buf[22], 0x00);
    }

    /* === Scenario 3: frame length constants ============================ */
    EXPECT_EQ(TELEM_WIRE_SAMPLE_LEN, 11);
    EXPECT_EQ(TELEM_WIRE_EVENT_LEN,  23);

    /* === Scenario 4: endianness sentinel =============================== */
    {
        uint8_t buf[TELEM_WIRE_SAMPLE_LEN];
        telem_wire_pack_sample(buf, 0, 0, (int32_t)0x01020304);
        /* value bytes are at offset 7..10 */
        EXPECT_EQ(buf[7],  0x04);
        EXPECT_EQ(buf[8],  0x03);
        EXPECT_EQ(buf[9],  0x02);
        EXPECT_EQ(buf[10], 0x01);
    }

    /* === Scenario 5: negative value (int32 twos-complement) =========== */
    {
        uint8_t buf[TELEM_WIRE_SAMPLE_LEN];
        telem_wire_pack_sample(buf, 0, 0, -1);
        /* -1 as int32 -> 0xFFFFFFFF */
        EXPECT_EQ(buf[7],  0xFF);
        EXPECT_EQ(buf[8],  0xFF);
        EXPECT_EQ(buf[9],  0xFF);
        EXPECT_EQ(buf[10], 0xFF);
    }
}
