/* test/unit/test_arbitrator.c -- unit tests for thermal_arbitrator_max_wins. */
#include <stdint.h>
#include "harness.h"
#include "thermal_arbitrator.h"

TEST_CASE(arbitrator_max_wins) {
    /* count == 0 -> 0 (defense-in-depth; validate_config rule 31 prevents
     * an actuator without any referencing zone in production). */
    EXPECT_EQ(thermal_arbitrator_max_wins(NULL, 0), 0);

    /* count == 1: returns the single element. */
    uint8_t one[] = { 42 };
    EXPECT_EQ(thermal_arbitrator_max_wins(one, 1), 42);

    uint8_t one_zero[] = { 0 };
    EXPECT_EQ(thermal_arbitrator_max_wins(one_zero, 1), 0);

    /* Max at position 0. */
    uint8_t at_front[] = { 200, 50, 75 };
    EXPECT_EQ(thermal_arbitrator_max_wins(at_front, 3), 200);

    /* Max in the middle. */
    uint8_t in_middle[] = { 30, 180, 60, 90 };
    EXPECT_EQ(thermal_arbitrator_max_wins(in_middle, 4), 180);

    /* Max at the last position. */
    uint8_t at_back[] = { 10, 20, 30, 40, 250 };
    EXPECT_EQ(thermal_arbitrator_max_wins(at_back, 5), 250);

    /* Tie of maxes: that value still wins. */
    uint8_t tied[] = { 100, 200, 200, 100 };
    EXPECT_EQ(thermal_arbitrator_max_wins(tied, 4), 200);

    /* All zeros. */
    uint8_t zeros[] = { 0, 0, 0, 0 };
    EXPECT_EQ(thermal_arbitrator_max_wins(zeros, 4), 0);

    /* 255 anywhere wins (boundary). */
    uint8_t with_max_front[] = { 255, 0, 0 };
    EXPECT_EQ(thermal_arbitrator_max_wins(with_max_front, 3), 255);

    uint8_t with_max_back[] = { 0, 0, 255 };
    EXPECT_EQ(thermal_arbitrator_max_wins(with_max_back, 3), 255);
}
