/* test/unit/test_curve.c
 *
 * Unit tests for thermal_curve_eval_y0/y1. Covers endpoint clamping,
 * knot-exact matches, midpoint interpolation, two-point degenerate,
 * eight-segment max, C99 truncation toward zero, and y0/y1 axis
 * independence.
 */
#include "harness.h"
#include "thermal_curve.h"
#include "thermal_types.h"
#include "thermal_config.h"

/* Canonical acoustic_mask curve from PRD §5.1 (matches replay/golden). */
static const thermal_curve_point_t ACOUSTIC[] = {
    {   0, 120,     0 },
    {  30, 180,     0 },
    {  80, 255, -5000 },
    { 130, 255, -8000 },
};
#define ACOUSTIC_COUNT ((uint8_t)(sizeof(ACOUSTIC) / sizeof(ACOUSTIC[0])))

TEST_CASE(curve_eval) {
    /* === Endpoint clamping === */
    EXPECT_EQ(thermal_curve_eval_y0(ACOUSTIC, ACOUSTIC_COUNT, -100), 120);
    EXPECT_EQ(thermal_curve_eval_y1(ACOUSTIC, ACOUSTIC_COUNT, -100), 0);
    EXPECT_EQ(thermal_curve_eval_y0(ACOUSTIC, ACOUSTIC_COUNT, 200), 255);
    EXPECT_EQ(thermal_curve_eval_y1(ACOUSTIC, ACOUSTIC_COUNT, 200), -8000);

    /* === Knot-exact matches (no interpolation needed) === */
    EXPECT_EQ(thermal_curve_eval_y0(ACOUSTIC, ACOUSTIC_COUNT, 0), 120);
    EXPECT_EQ(thermal_curve_eval_y1(ACOUSTIC, ACOUSTIC_COUNT, 0), 0);
    EXPECT_EQ(thermal_curve_eval_y0(ACOUSTIC, ACOUSTIC_COUNT, 30), 180);
    EXPECT_EQ(thermal_curve_eval_y1(ACOUSTIC, ACOUSTIC_COUNT, 80), -5000);
    EXPECT_EQ(thermal_curve_eval_y0(ACOUSTIC, ACOUSTIC_COUNT, 130), 255);

    /* === Midpoint interpolation === */
    /* [0,30] y0: 120 + (15*60)/30 = 150 */
    EXPECT_EQ(thermal_curve_eval_y0(ACOUSTIC, ACOUSTIC_COUNT, 15), 150);
    /* Same midpoint, y1 axis flat at 0 */
    EXPECT_EQ(thermal_curve_eval_y1(ACOUSTIC, ACOUSTIC_COUNT, 15), 0);
    /* [80,130] y0 flat at 255 */
    EXPECT_EQ(thermal_curve_eval_y0(ACOUSTIC, ACOUSTIC_COUNT, 105), 255);
    /* [80,130] y1: -5000 + (25 * -3000)/50 = -6500 */
    EXPECT_EQ(thermal_curve_eval_y1(ACOUSTIC, ACOUSTIC_COUNT, 105), -6500);

    /* === Two-point degenerate === */
    static const thermal_curve_point_t TWOPT[] = {
        {   0, 100, -50 },
        { 100, 200,  50 },
    };
    EXPECT_EQ(thermal_curve_eval_y0(TWOPT, 2, -1), 100);
    EXPECT_EQ(thermal_curve_eval_y1(TWOPT, 2, 101), 50);
    /* Midpoint y0: 100 + (50 * 100)/100 = 150 */
    EXPECT_EQ(thermal_curve_eval_y0(TWOPT, 2, 50), 150);
    /* Midpoint y1: -50 + (50 * 100)/100 = 0 */
    EXPECT_EQ(thermal_curve_eval_y1(TWOPT, 2, 50), 0);

    /* === C99 truncation toward zero (synthetic curve) ===
     * (0,0,0) -> (3,0,-10). At x=1: (1 * -10)/3 = -10/3.
     * C99 trunc toward zero: -3. Python's // would floor to -4. */
    static const thermal_curve_point_t TRUNC[] = {
        { 0, 0,   0 },
        { 3, 0, -10 },
    };
    EXPECT_EQ(thermal_curve_eval_y1(TRUNC, 2, 1), -3);
    /* At x=2: (2 * -10)/3 = -20/3. C99 trunc: -6. */
    EXPECT_EQ(thermal_curve_eval_y1(TRUNC, 2, 2), -6);

    /* === Degenerate counts (defensive contract) === */
    EXPECT_EQ(thermal_curve_eval_y0(NULL, 0, 42), 0);
    EXPECT_EQ(thermal_curve_eval_y1(NULL, 0, -42), 0);
    static const thermal_curve_point_t ONE[] = { { 50, 77, -33 } };
    EXPECT_EQ(thermal_curve_eval_y0(ONE, 1, 0), 77);
    EXPECT_EQ(thermal_curve_eval_y0(ONE, 1, 999), 77);
    EXPECT_EQ(thermal_curve_eval_y1(ONE, 1, -100), -33);

    /* === Eight-segment max (THERMAL_MAX_CURVE_POINTS = 8) === */
    static const thermal_curve_point_t MAX8[] = {
        {  0,  0, 0 }, { 10, 10, 0 }, { 20, 20, 0 }, { 30, 30, 0 },
        { 40, 40, 0 }, { 50, 50, 0 }, { 60, 60, 0 }, { 70, 70, 0 },
    };
    EXPECT_EQ(thermal_curve_eval_y0(MAX8, 8, 25), 25);
    EXPECT_EQ(thermal_curve_eval_y0(MAX8, 8, 5),   5);
    EXPECT_EQ(thermal_curve_eval_y0(MAX8, 8, 65), 65);
    EXPECT_EQ(thermal_curve_eval_y0(MAX8, 8, 100), 70);   /* clamp above */
}
