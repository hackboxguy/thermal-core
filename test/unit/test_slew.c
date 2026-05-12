/* test/unit/test_slew.c -- unit tests for thermal_slew_step. */
#include <stdint.h>
#include "harness.h"
#include "thermal_slew.h"

TEST_CASE(slew_step) {
    thermal_slew_result_t r;

    /* Request equals prev: pass-through, not limited. */
    thermal_slew_step(100, 100, 8, 0, &r);
    EXPECT_EQ(r.new_duty, 100);
    EXPECT_EQ(r.slew_limited, 0);

    /* Upward within step (delta < slew_per_tick): pass-through. */
    thermal_slew_step(100, 105, 8, 0, &r);
    EXPECT_EQ(r.new_duty, 105);
    EXPECT_EQ(r.slew_limited, 0);

    /* Upward beyond step: clamped to prev + slew_per_tick. */
    thermal_slew_step(100, 200, 8, 0, &r);
    EXPECT_EQ(r.new_duty, 108);
    EXPECT_EQ(r.slew_limited, 1);

    /* Downward beyond step: clamped to prev - slew_per_tick. */
    thermal_slew_step(200, 100, 8, 0, &r);
    EXPECT_EQ(r.new_duty, 192);
    EXPECT_EQ(r.slew_limited, 1);

    /* Edge: delta == slew_per_tick (upward) is allowed -- pass-through. */
    thermal_slew_step(100, 108, 8, 0, &r);
    EXPECT_EQ(r.new_duty, 108);
    EXPECT_EQ(r.slew_limited, 0);

    /* Edge: delta == -slew_per_tick (downward) is allowed -- pass-through. */
    thermal_slew_step(108, 100, 8, 0, &r);
    EXPECT_EQ(r.new_duty, 100);
    EXPECT_EQ(r.slew_limited, 0);

    /* slew_per_tick == 0: documented no-limit. */
    thermal_slew_step(100, 200, 0, 0, &r);
    EXPECT_EQ(r.new_duty, 200);
    EXPECT_EQ(r.slew_limited, 0);
    thermal_slew_step(200, 0, 0, 0, &r);
    EXPECT_EQ(r.new_duty, 0);
    EXPECT_EQ(r.slew_limited, 0);

    /* safety_override_up + upward: bypasses slew (PRD §4.6 line 598). */
    thermal_slew_step(100, 255, 8, 1, &r);
    EXPECT_EQ(r.new_duty, 255);
    EXPECT_EQ(r.slew_limited, 0);

    /* safety_override_up + downward: STILL clamped (downward obeys slew). */
    thermal_slew_step(255, 100, 8, 1, &r);
    EXPECT_EQ(r.new_duty, 247);
    EXPECT_EQ(r.slew_limited, 1);

    /* safety_override_up + equal: pass-through. */
    thermal_slew_step(150, 150, 8, 1, &r);
    EXPECT_EQ(r.new_duty, 150);
    EXPECT_EQ(r.slew_limited, 0);

    /* safety_override_up + upward by exactly 1: bypass still applies (request
     * is strictly upward). */
    thermal_slew_step(100, 101, 8, 1, &r);
    EXPECT_EQ(r.new_duty, 101);
    EXPECT_EQ(r.slew_limited, 0);

    /* Boundary: prev=0, requested=255, override -> 255. */
    thermal_slew_step(0, 255, 8, 1, &r);
    EXPECT_EQ(r.new_duty, 255);
    EXPECT_EQ(r.slew_limited, 0);

    /* Boundary: prev=255, requested=0, normal slew -> 247. */
    thermal_slew_step(255, 0, 8, 0, &r);
    EXPECT_EQ(r.new_duty, 247);
    EXPECT_EQ(r.slew_limited, 1);
}
