/* test/unit/test_filter.c
 *
 * Unit tests for thermal_filter_step + thermal_filter_reset. Covers
 * lifecycle (pre-init, first-valid init, valid->invalid->valid resume),
 * math (alpha=0, alpha=Q16_ONE, half-alpha, geometric convergence,
 * impulse response), arithmetic-shift truncation toward -inf, and Q16.16
 * saturation at INT32_MAX/MIN.
 */
#include <stdint.h>
#include "harness.h"
#include "thermal_filter.h"
#include "thermal_config.h"

TEST_CASE(filter) {
    thermal_filter_state_t s;

    /* === Lifecycle: pre-init === */
    thermal_filter_reset(&s);
    EXPECT_EQ(s.valid, 0);
    EXPECT_EQ(s.initialized, 0);
    EXPECT_EQ(s.filtered_value, 0);

    /* === Lifecycle: first valid sample initializes directly === */
    thermal_filter_step(&s, 16384, 75000, 1);
    EXPECT_EQ(s.filtered_value, 75000);
    EXPECT_EQ(s.valid, 1);
    EXPECT_EQ(s.initialized, 1);

    /* === alpha = 0 holds previous === */
    thermal_filter_step(&s, 0, 80000, 1);
    EXPECT_EQ(s.filtered_value, 75000);
    EXPECT_EQ(s.valid, 1);

    /* === alpha = Q16_ONE passes through === */
    thermal_filter_step(&s, Q16_ONE, 80000, 1);
    EXPECT_EQ(s.filtered_value, 80000);

    /* === Intermediate alpha: half-step toward sample === */
    thermal_filter_reset(&s);
    thermal_filter_step(&s, Q16_ONE, 0, 1);     /* init at 0 */
    thermal_filter_step(&s, 0x8000, 100, 1);    /* alpha = 0.5 */
    EXPECT_EQ(s.filtered_value, 50);

    /* === Geometric convergence: 100 steps with alpha=0.25 toward 1000 === */
    thermal_filter_reset(&s);
    thermal_filter_step(&s, Q16_ONE, 0, 1);
    for (int n = 0; n < 100; n++) {
        thermal_filter_step(&s, 16384, 1000, 1);
    }
    /* Should be within a few units of 1000 by step 100. */
    EXPECT_LE(1000 - s.filtered_value, 5);

    /* === Impulse response: half-alpha, single sample then revert === */
    thermal_filter_reset(&s);
    thermal_filter_step(&s, Q16_ONE, 0, 1);
    thermal_filter_step(&s, 0x8000, 1000, 1);
    EXPECT_EQ(s.filtered_value, 500);
    thermal_filter_step(&s, 0x8000, 0, 1);
    EXPECT_EQ(s.filtered_value, 250);

    /* === Q16.16 saturation: alpha > Q16_ONE pushes past INT32_MAX === */
    thermal_filter_reset(&s);
    thermal_filter_step(&s, Q16_ONE, INT32_MAX - 100, 1);  /* init */
    /* alpha = 4 * Q16_ONE, sample - prev = 50:
     *   delta = (4 * Q16_ONE * 50) >> 16 = 200
     *   next  = (INT32_MAX - 100) + 200 = INT32_MAX + 100
     * Clamp to INT32_MAX. */
    thermal_filter_step(&s, 4 * Q16_ONE, INT32_MAX - 50, 1);
    EXPECT_EQ(s.filtered_value, INT32_MAX);

    /* === Negative saturation === */
    thermal_filter_reset(&s);
    thermal_filter_step(&s, Q16_ONE, INT32_MIN + 100, 1);
    /* alpha = 4 * Q16_ONE, sample - prev = -50:
     *   delta = (4 * Q16_ONE * -50) >> 16 = -200 (arithmetic shift floor)
     *   next  = (INT32_MIN + 100) - 200 = INT32_MIN - 100
     * Clamp to INT32_MIN. */
    thermal_filter_step(&s, 4 * Q16_ONE, INT32_MIN + 50, 1);
    EXPECT_EQ(s.filtered_value, INT32_MIN);

    /* === Invalid sample holds filtered_value, resets valid only === */
    thermal_filter_reset(&s);
    thermal_filter_step(&s, 16384, 75000, 1);
    thermal_filter_step(&s, 16384, 0, 0);
    EXPECT_EQ(s.filtered_value, 75000);
    EXPECT_EQ(s.valid, 0);
    EXPECT_EQ(s.initialized, 1);

    /* === valid -> invalid -> invalid -> valid: IIR resumes (not re-init) ===
     * Prev = 1000, sample = 5000, alpha = 0.25:
     *   delta = (0x4000 * 4000) >> 16 = 1000; next = 2000. */
    thermal_filter_reset(&s);
    thermal_filter_step(&s, 16384, 1000, 1);
    thermal_filter_step(&s, 16384, 9999, 0);
    thermal_filter_step(&s, 16384, 9999, 0);
    EXPECT_EQ(s.filtered_value, 1000);
    EXPECT_EQ(s.valid, 0);
    thermal_filter_step(&s, 16384, 5000, 1);
    EXPECT_EQ(s.filtered_value, 2000);
    EXPECT_EQ(s.valid, 1);

    /* === Round-half-away-from-zero: sub-LSB deltas round to 0 both ways ===
     * alpha = 0.25, |sample - prev| = 1:
     *   alpha * (+/-1) = +/-16384  (+/-0.25 LSB)  -> round-half-away -> 0
     * so a filter within 1 LSB of target holds, with no direction bias
     * (the old `>> 16` floor crept -1 on the negative side only). */
    thermal_filter_reset(&s);
    thermal_filter_step(&s, Q16_ONE, 100, 1);
    thermal_filter_step(&s, 16384, 99, 1);    /* -0.25 LSB -> 0 */
    EXPECT_EQ(s.filtered_value, 100);
    thermal_filter_step(&s, 16384, 101, 1);   /* +0.25 LSB -> 0 */
    EXPECT_EQ(s.filtered_value, 100);

    /* === Invalid as the very first sample leaves filter uninitialized === */
    thermal_filter_reset(&s);
    thermal_filter_step(&s, 16384, 42, 0);
    EXPECT_EQ(s.initialized, 0);
    EXPECT_EQ(s.valid, 0);
    EXPECT_EQ(s.filtered_value, 0);
    thermal_filter_step(&s, 16384, 77, 1);   /* now genuinely "first valid" */
    EXPECT_EQ(s.filtered_value, 77);
    EXPECT_EQ(s.valid, 1);
    EXPECT_EQ(s.initialized, 1);
}
