/* test/unit/test_harness.c
 *
 * Stage 0 trivial test. Proves the harness compiles, links, runs, and that
 * the cross-directory include from test/unit/ into core/ works under -Werror
 * -pedantic. No real module is exercised yet.
 */
#include "harness.h"
#include "thermal_config.h"

TEST_CASE(harness_works) {
    EXPECT_EQ(1, 1);
    /* Profile-agnostic: proves the cross-directory include of
     * thermal_config.h resolved without pinning the default-profile
     * value (a tiny-profile build overrides THERMAL_MAX_ZONES). */
    EXPECT_EQ(THERMAL_MAX_ZONES >= 1, 1);
}
