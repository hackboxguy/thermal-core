/* test/unit/test_zone.c
 *
 * Unit tests for thermal_zone_aggregate. Covers all three modes
 * (MAX, AVG, WEIGHTED), partial-validity skipping, all-invalid
 * fallback, edge weights (zero, dominant), single-sensor, and the
 * count == 0 defensive case.
 */
#include <stdint.h>
#include "harness.h"
#include "thermal_zone.h"
#include "thermal_config.h"  /* for Q16_ONE */

TEST_CASE(zone_aggregate) {
    thermal_zone_aggregate_result_t r;

    /* === MAX === */
    {
        int32_t v[3]      = { 60000, 75000, 70000 };
        uint8_t valid[3]  = { 1, 1, 1 };
        thermal_zone_aggregate(THERMAL_AGG_MAX, v, valid, NULL, 3, 85000, &r);
        EXPECT_EQ(r.temp_mc, 75000);
        EXPECT_EQ(r.valid, 1);
    }

    /* MAX: partial-invalid skips, returns max of valid */
    {
        int32_t v[3]      = { 60000, 99999, 70000 };  /* idx 1 is invalid */
        uint8_t valid[3]  = { 1, 0, 1 };
        thermal_zone_aggregate(THERMAL_AGG_MAX, v, valid, NULL, 3, 85000, &r);
        EXPECT_EQ(r.temp_mc, 70000);
        EXPECT_EQ(r.valid, 1);
    }

    /* MAX: all invalid -> fallback */
    {
        int32_t v[3]      = { 60000, 75000, 70000 };
        uint8_t valid[3]  = { 0, 0, 0 };
        thermal_zone_aggregate(THERMAL_AGG_MAX, v, valid, NULL, 3, 85000, &r);
        EXPECT_EQ(r.temp_mc, 85000);
        EXPECT_EQ(r.valid, 0);
    }

    /* MAX: negative values handled correctly */
    {
        int32_t v[3]      = { -10000, -5000, -7000 };
        uint8_t valid[3]  = { 1, 1, 1 };
        thermal_zone_aggregate(THERMAL_AGG_MAX, v, valid, NULL, 3, 0, &r);
        EXPECT_EQ(r.temp_mc, -5000);
        EXPECT_EQ(r.valid, 1);
    }

    /* MAX: single valid sensor */
    {
        int32_t v[1]      = { 50000 };
        uint8_t valid[1]  = { 1 };
        thermal_zone_aggregate(THERMAL_AGG_MAX, v, valid, NULL, 1, 0, &r);
        EXPECT_EQ(r.temp_mc, 50000);
        EXPECT_EQ(r.valid, 1);
    }

    /* === AVG === */
    {
        int32_t v[3]      = { 60000, 70000, 80000 };
        uint8_t valid[3]  = { 1, 1, 1 };
        thermal_zone_aggregate(THERMAL_AGG_AVG, v, valid, NULL, 3, 0, &r);
        EXPECT_EQ(r.temp_mc, 70000);
        EXPECT_EQ(r.valid, 1);
    }

    /* AVG: partial-invalid drops out of denominator */
    {
        int32_t v[3]      = { 60000, 99999, 80000 };  /* idx 1 invalid */
        uint8_t valid[3]  = { 1, 0, 1 };
        thermal_zone_aggregate(THERMAL_AGG_AVG, v, valid, NULL, 3, 0, &r);
        EXPECT_EQ(r.temp_mc, 70000);  /* (60000 + 80000) / 2 */
        EXPECT_EQ(r.valid, 1);
    }

    /* AVG: all invalid -> fallback */
    {
        int32_t v[3]      = { 60000, 70000, 80000 };
        uint8_t valid[3]  = { 0, 0, 0 };
        thermal_zone_aggregate(THERMAL_AGG_AVG, v, valid, NULL, 3, 42000, &r);
        EXPECT_EQ(r.temp_mc, 42000);
        EXPECT_EQ(r.valid, 0);
    }

    /* AVG: C99 truncation toward zero for negative mean.
     * sum = -3, n = 3, -3 / 3 = -1. */
    {
        int32_t v[3]      = { -1, -1, -1 };
        uint8_t valid[3]  = { 1, 1, 1 };
        thermal_zone_aggregate(THERMAL_AGG_AVG, v, valid, NULL, 3, 0, &r);
        EXPECT_EQ(r.temp_mc, -1);
        EXPECT_EQ(r.valid, 1);
    }

    /* === WEIGHTED === */
    /* Equal weights -> same as AVG. (60000+75000+70000)/3 = 68333 trunc. */
    {
        int32_t v[3]      = { 60000, 75000, 70000 };
        uint8_t valid[3]  = { 1, 1, 1 };
        int32_t w[3]      = { Q16_ONE, Q16_ONE, Q16_ONE };
        thermal_zone_aggregate(THERMAL_AGG_WEIGHTED, v, valid, w, 3, 0, &r);
        EXPECT_EQ(r.temp_mc, 68333);
        EXPECT_EQ(r.valid, 1);
    }

    /* WEIGHTED: dominant weight on idx 1 pulls toward 75000.
     * (60000 + 75000*2 + 70000) / 4 = 280000 / 4 = 70000. */
    {
        int32_t v[3]      = { 60000, 75000, 70000 };
        uint8_t valid[3]  = { 1, 1, 1 };
        int32_t w[3]      = { Q16_ONE, 2 * Q16_ONE, Q16_ONE };
        thermal_zone_aggregate(THERMAL_AGG_WEIGHTED, v, valid, w, 3, 0, &r);
        EXPECT_EQ(r.temp_mc, 70000);
        EXPECT_EQ(r.valid, 1);
    }

    /* WEIGHTED: zero weight on idx 1 -> idx 1 effectively dropped.
     * (60000 + 0 + 80000) / (Q16_ONE + 0 + Q16_ONE) = 140000/2 = 70000. */
    {
        int32_t v[3]      = { 60000, 99999, 80000 };
        uint8_t valid[3]  = { 1, 1, 1 };
        int32_t w[3]      = { Q16_ONE, 0, Q16_ONE };
        thermal_zone_aggregate(THERMAL_AGG_WEIGHTED, v, valid, w, 3, 0, &r);
        EXPECT_EQ(r.temp_mc, 70000);
        EXPECT_EQ(r.valid, 1);
    }

    /* WEIGHTED: partial-invalid sensor dropped before weighting. */
    {
        int32_t v[3]      = { 60000, 99999, 80000 };  /* idx 1 invalid */
        uint8_t valid[3]  = { 1, 0, 1 };
        int32_t w[3]      = { Q16_ONE, Q16_ONE, Q16_ONE };
        thermal_zone_aggregate(THERMAL_AGG_WEIGHTED, v, valid, w, 3, 0, &r);
        EXPECT_EQ(r.temp_mc, 70000);  /* (60000 + 80000) / 2 */
        EXPECT_EQ(r.valid, 1);
    }

    /* WEIGHTED: all weights zero -> defensive fallback. */
    {
        int32_t v[3]      = { 60000, 75000, 70000 };
        uint8_t valid[3]  = { 1, 1, 1 };
        int32_t w[3]      = { 0, 0, 0 };
        thermal_zone_aggregate(THERMAL_AGG_WEIGHTED, v, valid, w, 3, 42000, &r);
        EXPECT_EQ(r.temp_mc, 42000);
        EXPECT_EQ(r.valid, 0);
    }

    /* === Defensive: count == 0 returns fallback === */
    {
        thermal_zone_aggregate(THERMAL_AGG_MAX, NULL, NULL, NULL, 0, 12345, &r);
        EXPECT_EQ(r.temp_mc, 12345);
        EXPECT_EQ(r.valid, 0);
    }
}
