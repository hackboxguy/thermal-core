/* test/unit/test_modifier.c -- unit tests for thermal_modifier_eval. */
#include <stdint.h>
#include <string.h>
#include "harness.h"
#include "thermal_modifier.h"
#include "thermal_core.h"
#include "thermal_types.h"

static void make_acoustic_mask_cfg(thermal_modifier_cfg_t *cfg) {
    /* From PRD §5.1 acoustic_mask example: 4 curve points across
     * speed 0..130 km/h, pwm_cap 120..255, trip_offset 0..-8000 mc. */
    memset(cfg, 0, sizeof(*cfg));
    cfg->context_id = 0;
    cfg->stages = THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET |
                  THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP;
    cfg->curve_count = 4;
    cfg->curve[0].x = 0;   cfg->curve[0].value0 = 120; cfg->curve[0].value1 = 0;
    cfg->curve[1].x = 30;  cfg->curve[1].value0 = 180; cfg->curve[1].value1 = 0;
    cfg->curve[2].x = 80;  cfg->curve[2].value0 = 255; cfg->curve[2].value1 = -5000;
    cfg->curve[3].x = 130; cfg->curve[3].value0 = 255; cfg->curve[3].value1 = -8000;
    cfg->fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;
}

TEST_CASE(modifier_eval) {
    thermal_modifier_cfg_t cfg;
    make_acoustic_mask_cfg(&cfg);
    thermal_modifier_eval_result_t r;

    /* Clamp below: speed -10 returns first-point values. */
    thermal_modifier_eval(&cfg, -10, &r);
    EXPECT_EQ(r.pwm_cap, 120);
    EXPECT_EQ(r.trip_offset_mc, 0);

    /* Knot-exact: speed 0. */
    thermal_modifier_eval(&cfg, 0, &r);
    EXPECT_EQ(r.pwm_cap, 120);
    EXPECT_EQ(r.trip_offset_mc, 0);

    /* Knot-exact: speed 30. */
    thermal_modifier_eval(&cfg, 30, &r);
    EXPECT_EQ(r.pwm_cap, 180);
    EXPECT_EQ(r.trip_offset_mc, 0);

    /* Interpolated mid-segment: speed 15 (between 0 and 30).
     * pwm_cap: 120 + (15 * 60)/30 = 150. trip_offset stays 0. */
    thermal_modifier_eval(&cfg, 15, &r);
    EXPECT_EQ(r.pwm_cap, 150);
    EXPECT_EQ(r.trip_offset_mc, 0);

    /* Speed 105: middle of segment 2-3.
     * pwm_cap flat at 255. trip_offset: -5000 + (25*-3000)/50 = -6500. */
    thermal_modifier_eval(&cfg, 105, &r);
    EXPECT_EQ(r.pwm_cap, 255);
    EXPECT_EQ(r.trip_offset_mc, -6500);

    /* Clamp above: speed 200. */
    thermal_modifier_eval(&cfg, 200, &r);
    EXPECT_EQ(r.pwm_cap, 255);
    EXPECT_EQ(r.trip_offset_mc, -8000);

    /* PRD §5.1 example sanity at speed 80 (knot 2): */
    thermal_modifier_eval(&cfg, 80, &r);
    EXPECT_EQ(r.pwm_cap, 255);
    EXPECT_EQ(r.trip_offset_mc, -5000);

    /* PRD §5.1 example sanity at speed 130 (last knot): */
    thermal_modifier_eval(&cfg, 130, &r);
    EXPECT_EQ(r.pwm_cap, 255);
    EXPECT_EQ(r.trip_offset_mc, -8000);
}
