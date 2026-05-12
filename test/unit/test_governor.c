/* test/unit/test_governor.c
 *
 * Unit tests for thermal_governor_step_wise. Covers single-trip enter/exit
 * with hysteresis dead-band hold (both directions), zero-hysteresis instant
 * flip, multi-trip ordering ("max cooling_state wins, not max-index"),
 * trip_count == 0, and stale prev-mask bit handling.
 */
#include <stdint.h>
#include "harness.h"
#include "thermal_governor.h"
#include "thermal_core.h"     /* for thermal_trip_cfg_t */
#include "thermal_types.h"    /* for THERMAL_TRIP_* severity enum */

TEST_CASE(governor_step_wise) {
    thermal_governor_step_result_t r;

    /* Single trip, hyst = 2000 mc, cooling_state = 2. */
    static const thermal_trip_cfg_t ONE_TRIP[] = {
        { 70000, 2000, THERMAL_TRIP_WARN, 2 },
    };

    /* Below trip temp, no previous activity: inactive */
    thermal_governor_step_wise(60000, ONE_TRIP, 1, 0, &r);
    EXPECT_EQ(r.active_trip_mask, 0);
    EXPECT_EQ(r.cooling_state, 0);

    /* At trip temp exactly: enters active */
    thermal_governor_step_wise(70000, ONE_TRIP, 1, 0, &r);
    EXPECT_EQ(r.active_trip_mask, 0x1);
    EXPECT_EQ(r.cooling_state, 2);

    /* Above trip temp: active */
    thermal_governor_step_wise(75000, ONE_TRIP, 1, 0, &r);
    EXPECT_EQ(r.active_trip_mask, 0x1);
    EXPECT_EQ(r.cooling_state, 2);

    /* Dead-band hold: was active, now in dead band: stays active */
    thermal_governor_step_wise(69000, ONE_TRIP, 1, /*prev*/0x1, &r);
    EXPECT_EQ(r.active_trip_mask, 0x1);
    EXPECT_EQ(r.cooling_state, 2);

    /* Dead-band hold: was inactive, now in dead band: stays inactive */
    thermal_governor_step_wise(69000, ONE_TRIP, 1, /*prev*/0x0, &r);
    EXPECT_EQ(r.active_trip_mask, 0);
    EXPECT_EQ(r.cooling_state, 0);

    /* Exit below cold threshold (70000 - 2000 = 68000): inactive regardless of prev */
    thermal_governor_step_wise(67999, ONE_TRIP, 1, /*prev*/0x1, &r);
    EXPECT_EQ(r.active_trip_mask, 0);
    EXPECT_EQ(r.cooling_state, 0);

    /* At cold threshold exactly (68000): in dead band (>= trip-hyst), hold */
    thermal_governor_step_wise(68000, ONE_TRIP, 1, /*prev*/0x1, &r);
    EXPECT_EQ(r.active_trip_mask, 0x1);

    /* Zero hysteresis: flips exactly at temp_mc, no dead band */
    static const thermal_trip_cfg_t ZERO_HYST[] = {
        { 70000, 0, THERMAL_TRIP_WARN, 1 },
    };
    thermal_governor_step_wise(69999, ZERO_HYST, 1, /*prev*/0x1, &r);
    EXPECT_EQ(r.active_trip_mask, 0);
    thermal_governor_step_wise(70000, ZERO_HYST, 1, 0, &r);
    EXPECT_EQ(r.active_trip_mask, 0x1);

    /* Multi-trip: cooling_states ascending with trip temperature */
    static const thermal_trip_cfg_t MULTI[] = {
        { 70000, 2000, THERMAL_TRIP_WARN,     1 },
        { 80000, 2000, THERMAL_TRIP_CRITICAL, 3 },
        { 90000, 2000, THERMAL_TRIP_SHUTDOWN, 4 },
    };

    thermal_governor_step_wise(50000, MULTI, 3, 0, &r);
    EXPECT_EQ(r.active_trip_mask, 0);
    EXPECT_EQ(r.cooling_state, 0);

    thermal_governor_step_wise(75000, MULTI, 3, 0, &r);
    EXPECT_EQ(r.active_trip_mask, 0x1);
    EXPECT_EQ(r.cooling_state, 1);

    thermal_governor_step_wise(85000, MULTI, 3, 0, &r);
    EXPECT_EQ(r.active_trip_mask, 0x3);
    EXPECT_EQ(r.cooling_state, 3);

    thermal_governor_step_wise(95000, MULTI, 3, 0, &r);
    EXPECT_EQ(r.active_trip_mask, 0x7);
    EXPECT_EQ(r.cooling_state, 4);

    /* Cooling state max picked across array, not by index order */
    static const thermal_trip_cfg_t REORDERED[] = {
        { 70000, 1000, THERMAL_TRIP_WARN,     5 },   /* highest cs in slot 0 */
        { 80000, 1000, THERMAL_TRIP_CRITICAL, 2 },
    };
    thermal_governor_step_wise(85000, REORDERED, 2, 0, &r);
    EXPECT_EQ(r.active_trip_mask, 0x3);
    EXPECT_EQ(r.cooling_state, 5);

    /* trip_count == 0: NULL trips array allowed */
    thermal_governor_step_wise(99999, NULL, 0, /*prev*/0xFFFFFFFFu, &r);
    EXPECT_EQ(r.active_trip_mask, 0);
    EXPECT_EQ(r.cooling_state, 0);

    /* Stale bits in prev_active_mask beyond trip_count: ignored */
    thermal_governor_step_wise(50000, MULTI, 2, /*prev*/0xFFu, &r);
    EXPECT_EQ(r.active_trip_mask, 0);

    /* Mid-dead-band hold preserves multi-trip mask correctly.
     * Trip 0 hot-side = 70000, cold-side = 68000.
     * Trip 1 hot-side = 80000, cold-side = 78000.
     * At 79000: trip 0 well above cold; trip 1 in dead band.
     * Prev = 0x3 -> trip 1 holds active. */
    thermal_governor_step_wise(79000, MULTI, 3, /*prev*/0x3u, &r);
    EXPECT_EQ(r.active_trip_mask, 0x3);
    EXPECT_EQ(r.cooling_state, 3);
}
