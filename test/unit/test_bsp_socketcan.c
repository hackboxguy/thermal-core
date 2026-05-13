/* test/unit/test_bsp_socketcan.c
 *
 * Stage 11 11b — bsp_socketcan unit tests.
 *
 * Drives the state struct directly with synthetic CAN frames; no
 * socket I/O.  Closes out the impl-plan §5 Stage 11 unit-test
 * surface for the response-timeout + fail_safe path.
 *
 * Scenarios:
 *   1. Valid 120 km/h response on 0x7E8 -> cache updates.
 *   2. Frame on wrong CAN ID (0x123) -> cache untouched.
 *   3. NRC response (0x7F 0x01 0x12) on 0x7E8 -> cache untouched.
 *   4. read_into_sample within timeout after valid frame -> valid=1.
 *   5. read_into_sample on cold start -> valid=0 (no frame yet).
 *   6. read_into_sample after timeout -> valid=0 (PRD §6.3 fail-safe).
 *   7. Response with wrong PID echo -> cache untouched.
 *   8. Response truncated (len=4) -> cache untouched.
 */
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "bsp_socketcan.h"
#include "obd2.h"
#include "thermal_types.h"

#define VEHICLE_SPEED_PID  OBD2_PID_VEHICLE_SPEED
#define TIMEOUT_MS         3000u

/* Helper: 8-byte well-formed Service 01 PID 0x0D response carrying
 * the given speed value.  Lives at file scope so each scenario can
 * tweak one byte before passing it in. */
static void make_good_response(uint8_t out[8], uint8_t speed_kmh)
{
    out[0] = 0x03;
    out[1] = 0x41;
    out[2] = VEHICLE_SPEED_PID;
    out[3] = speed_kmh;
    out[4] = 0x55;
    out[5] = 0x55;
    out[6] = 0x55;
    out[7] = 0x55;
}

TEST_CASE(bsp_socketcan) {
    /* === Scenario 1: valid 120 km/h response updates cache ========== */
    {
        bsp_socketcan_state_t s;
        bsp_socketcan_state_init(&s, TIMEOUT_MS, VEHICLE_SPEED_PID);

        uint8_t data[8];
        make_good_response(data, 120);
        bsp_socketcan_handle_frame(&s, OBD2_RESPONSE_ID_ECU,
                                    data, sizeof(data), 5000u);
        EXPECT_EQ((int)s.ever_received, 1);
        EXPECT_EQ(s.last_value, 120);
        EXPECT_EQ((int)s.last_value_ms, 5000);
    }

    /* === Scenario 2: wrong CAN ID -> cache untouched ================ */
    /* PRD §6.1: only ECU responses on 0x7E8 are ours.  An unrelated
     * vehicle-bus frame at 0x123 must not poison the cache. */
    {
        bsp_socketcan_state_t s;
        bsp_socketcan_state_init(&s, TIMEOUT_MS, VEHICLE_SPEED_PID);

        uint8_t data[8];
        make_good_response(data, 200);
        bsp_socketcan_handle_frame(&s, 0x123u,
                                    data, sizeof(data), 5000u);
        EXPECT_EQ((int)s.ever_received, 0);
        EXPECT_EQ(s.last_value, 0);
        EXPECT_EQ((int)s.last_value_ms, 0);
    }

    /* === Scenario 3: NRC response -> cache untouched ================ */
    /* The ECU explicitly rejected our request (NRC 0x12).  BSP
     * treats this the same as "no fresh value": let the staleness
     * window expire. */
    {
        bsp_socketcan_state_t s;
        bsp_socketcan_state_init(&s, TIMEOUT_MS, VEHICLE_SPEED_PID);

        const uint8_t nrc[8] = {
            0x03, OBD2_NEGATIVE_RESPONSE, OBD2_SERVICE_01_CURRENT_DATA,
            0x12, 0x55, 0x55, 0x55, 0x55
        };
        bsp_socketcan_handle_frame(&s, OBD2_RESPONSE_ID_ECU,
                                    nrc, sizeof(nrc), 5000u);
        EXPECT_EQ((int)s.ever_received, 0);
        EXPECT_EQ(s.last_value, 0);
    }

    /* === Scenario 4: read within timeout -> valid=1 ================= */
    /* now_ms = 7000, last_value_ms = 5000, age = 2000 < 3000 timeout. */
    {
        bsp_socketcan_state_t s;
        bsp_socketcan_state_init(&s, TIMEOUT_MS, VEHICLE_SPEED_PID);

        uint8_t data[8];
        make_good_response(data, 88);
        bsp_socketcan_handle_frame(&s, OBD2_RESPONSE_ID_ECU,
                                    data, sizeof(data), 5000u);

        thermal_sample_t out;
        memset(&out, 0, sizeof(out));
        int rc = bsp_socketcan_read_into_sample(&s, /*id*/ 7u, 7000u, &out);
        EXPECT_EQ(rc, 0);
        EXPECT_EQ((int)out.id, 7);
        EXPECT_EQ((int)out.valid, 1);
        EXPECT_EQ(out.value, 88);
        EXPECT_EQ((int)out.sample_ts_ms, 7000);
    }

    /* === Scenario 5: cold start -> valid=0 ========================== */
    /* PRD §6.3: before the first OK frame the BSP must report
     * stale rather than synthesising a zero "vehicle at rest". */
    {
        bsp_socketcan_state_t s;
        bsp_socketcan_state_init(&s, TIMEOUT_MS, VEHICLE_SPEED_PID);

        thermal_sample_t out;
        memset(&out, 0, sizeof(out));
        int rc = bsp_socketcan_read_into_sample(&s, 7u, 5000u, &out);
        EXPECT_EQ(rc, 0);
        EXPECT_EQ((int)out.valid, 0);
        EXPECT_EQ(out.value, 0);
    }

    /* === Scenario 6: stale after timeout -> valid=0 ================ */
    /* now_ms - last_value_ms = 5000 > 3000 timeout.  Drives the
     * acoustic_mask fail-safe assume_stationary fallback. */
    {
        bsp_socketcan_state_t s;
        bsp_socketcan_state_init(&s, TIMEOUT_MS, VEHICLE_SPEED_PID);

        uint8_t data[8];
        make_good_response(data, 120);
        bsp_socketcan_handle_frame(&s, OBD2_RESPONSE_ID_ECU,
                                    data, sizeof(data), 5000u);

        thermal_sample_t out;
        memset(&out, 0, sizeof(out));
        int rc = bsp_socketcan_read_into_sample(&s, 7u, 10000u, &out);
        EXPECT_EQ(rc, 0);
        EXPECT_EQ((int)out.valid, 0);
        EXPECT_EQ(out.value, 0);
    }

    /* === Scenario 7: wrong PID echo -> cache untouched ============= */
    {
        bsp_socketcan_state_t s;
        bsp_socketcan_state_init(&s, TIMEOUT_MS, VEHICLE_SPEED_PID);

        /* PID 0x0C echoed, but we asked for 0x0D. */
        const uint8_t data[8] = {
            0x03, 0x41, 0x0C, 99, 0x55, 0x55, 0x55, 0x55
        };
        bsp_socketcan_handle_frame(&s, OBD2_RESPONSE_ID_ECU,
                                    data, sizeof(data), 5000u);
        EXPECT_EQ((int)s.ever_received, 0);
        EXPECT_EQ(s.last_value, 0);
    }

    /* === Scenario 8: truncated frame -> cache untouched ============ */
    {
        bsp_socketcan_state_t s;
        bsp_socketcan_state_init(&s, TIMEOUT_MS, VEHICLE_SPEED_PID);

        /* len = 4 -> obd2 decoder returns ERR_BAD_SHAPE. */
        const uint8_t data[4] = { 0x03, 0x41, 0x0D, 120 };
        bsp_socketcan_handle_frame(&s, OBD2_RESPONSE_ID_ECU,
                                    data, sizeof(data), 5000u);
        EXPECT_EQ((int)s.ever_received, 0);
        EXPECT_EQ(s.last_value, 0);
    }
}
