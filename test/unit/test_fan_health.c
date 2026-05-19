/* test/unit/test_fan_health.c
 *
 * Unit tests for the fan-health detector (Stage 17, PRD Appendix C):
 * steady-window gating, skip-window suppression, signed delta_pct with
 * round-half-away-from-zero and saturation, off-baseline interpolation,
 * confidence gating, provenance gating, and the negative controls
 * (positive delta and low confidence both stay HEALTHY).
 */
#include "harness.h"
#include "thermal_fan_health.h"

#include <string.h>

/* Baseline: a clean linear RPM = 20 * PWM line, so the expected RPM at
 * any PWM (interpolated or at a knot) is trivial to hand-compute. */
static void make_cfg(thermal_fan_health_cfg_t *c) {
    memset(c, 0, sizeof(*c));
    c->enable                 = 1;
    c->baseline_source        = THERMAL_FAN_BASELINE_SRC_FIELD;
    c->baseline[0].x = 60;  c->baseline[0].value0 = 1200;
    c->baseline[1].x = 120; c->baseline[1].value0 = 2400;
    c->baseline[2].x = 180; c->baseline[2].value0 = 3600;
    c->baseline[3].x = 240; c->baseline[3].value0 = 4800;
    c->baseline_count         = 4;
    c->stable_pwm_ticks       = 3;
    c->stable_pwm_tolerance   = 2;
    c->stable_rpm_ticks       = 2;
    c->stable_rpm_tolerance_pct = 5;
    c->min_points_observed    = 2;
    c->aging_pct              = -5;
    c->degraded_pct           = -15;
    c->failing_pct            = -30;
}

/* Drive `n` steady ticks at a fixed operating point (valid, no skip). */
static void drive_point(thermal_fan_health_state_t *s,
                        const thermal_fan_health_cfg_t *c,
                        uint8_t pwm, uint16_t rpm, int n) {
    for (int i = 0; i < n; i++) {
        thermal_fan_health_step(s, c, pwm, rpm, 1, 0, 0, 0);
    }
}

TEST_CASE(fan_health) {
    thermal_fan_health_cfg_t   c;
    thermal_fan_health_state_t s;
    make_cfg(&c);

    /* === Steady-window gate: no fold before the windows are met === */
    thermal_fan_health_reset(&s);
    thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 0, 0);  /* sets ref */
    EXPECT_EQ(s.have_ref, 1);
    EXPECT_EQ(s.pwm_run, 1);
    EXPECT_EQ(s.sample_count[1], 0);
    thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 0, 0);  /* run=2 */
    EXPECT_EQ(s.pwm_run, 2);
    EXPECT_EQ(s.sample_count[1], 0);                          /* still none */
    thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 0, 0);  /* run=3 -> fold */
    EXPECT_EQ(s.sample_count[1], 1);

    /* === delta_pct at a baseline knot: measured 90% of expected === */
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 2160, 8);     /* 2160 = 0.90 * 2400 -> -10% */
    EXPECT_EQ(s.ema_x256[1], -10 * 256);
    EXPECT_EQ(s.confidence, 1);
    EXPECT_EQ(s.health_delta_pct, -10);

    /* === Confidence gate (negative control): one point is not enough ===
     * A single point at -40% must still report HEALTHY -- an
     * opportunistic detector does not escalate on one operating point. */
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 1440, 8);     /* 1440 = 0.60 * 2400 -> -40% */
    EXPECT_EQ(s.confidence, 1);
    EXPECT_EQ(s.severity, THERMAL_FAN_HEALTH_HEALTHY);
    drive_point(&s, &c, 180, 2160, 8);     /* 2160 = 0.60 * 3600 -> -40% */
    EXPECT_EQ(s.confidence, 2);
    EXPECT_EQ(s.health_delta_pct, -40);
    EXPECT_EQ(s.severity, THERMAL_FAN_HEALTH_FAILING);

    /* === Severity ladder at two confident points === */
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 1920, 8);     /* -20% */
    drive_point(&s, &c, 180, 2880, 8);     /* -20% */
    EXPECT_EQ(s.health_delta_pct, -20);
    EXPECT_EQ(s.severity, THERMAL_FAN_HEALTH_DEGRADED);

    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 2208, 8);     /* -8% */
    drive_point(&s, &c, 180, 3312, 8);     /* -8% */
    EXPECT_EQ(s.health_delta_pct, -8);
    EXPECT_EQ(s.severity, THERMAL_FAN_HEALTH_AGING);

    /* === Provenance gating: a model baseline suppresses AGING === */
    c.baseline_source = THERMAL_FAN_BASELINE_SRC_MODEL;
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 2208, 8);     /* -8% */
    drive_point(&s, &c, 180, 3312, 8);     /* -8% */
    EXPECT_EQ(s.health_delta_pct, -8);
    EXPECT_EQ(s.severity, THERMAL_FAN_HEALTH_HEALTHY);   /* AGING suppressed */
    /* ...but a model baseline still asserts the coarse end. */
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 1920, 8);     /* -20% */
    drive_point(&s, &c, 180, 2880, 8);     /* -20% */
    EXPECT_EQ(s.severity, THERMAL_FAN_HEALTH_DEGRADED);
    c.baseline_source = THERMAL_FAN_BASELINE_SRC_FIELD;

    /* === Negative control: a positive delta never escalates ===
     * Positive per-point drift is clamped out of the aggregate, so the
     * health score is 0 (not +20) and severity stays HEALTHY. */
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 2880, 8);     /* +20% */
    drive_point(&s, &c, 180, 4320, 8);     /* +20% */
    EXPECT_EQ(s.confidence, 2);
    EXPECT_EQ(s.health_delta_pct, 0);
    EXPECT_EQ(s.severity, THERMAL_FAN_HEALTH_HEALTHY);

    /* === Negative control: a positive point cannot cancel a negative
     * one. Point 1 reads -20%, point 2 reads +20%; the +20 is clamped
     * to 0 in the aggregate, so the -20 still surfaces (would average
     * to 0 -> HEALTHY without the clamp). */
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 1920, 8);     /* -20% */
    drive_point(&s, &c, 180, 4320, 8);     /* +20% */
    EXPECT_EQ(s.confidence, 2);
    EXPECT_EQ(s.health_delta_pct, -10);
    EXPECT_EQ(s.severity, THERMAL_FAN_HEALTH_AGING);

    /* === Off-baseline interpolation: PWM 90 -> expected 1800 === */
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 90, 1620, 8);      /* 1620 = 0.90 * 1800 -> -10% */
    /* PWM 90 is equidistant from knots 60 and 120; the tie resolves to
     * the lower index, so the delta lands on point 0. */
    EXPECT_EQ(s.ema_x256[0], -10 * 256);
    EXPECT_EQ(s.confidence, 1);

    /* === Round-half-away-from-zero on delta_pct === */
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 2340, 8);     /* -2.5% -> -3 */
    EXPECT_EQ(s.ema_x256[1], -3 * 256);
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 2460, 8);     /* +2.5% -> +3 */
    EXPECT_EQ(s.ema_x256[1], 3 * 256);

    /* === Saturation: a delta beyond +127% clamps === */
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 10000, 8);    /* +316% -> clamp to +127 */
    EXPECT_EQ(s.ema_x256[1], 127 * 256);

    /* === Skip windows: each skip cause folds nothing === */
    thermal_fan_health_reset(&s);
    for (int i = 0; i < 8; i++) {           /* tach invalid */
        thermal_fan_health_step(&s, &c, 120, 2400, 0, 0, 0, 0);
    }
    EXPECT_EQ(s.have_ref, 0);
    EXPECT_EQ(s.confidence, 0);

    thermal_fan_health_reset(&s);
    for (int i = 0; i < 8; i++) {           /* slew-limited */
        thermal_fan_health_step(&s, &c, 120, 2400, 1, 1, 0, 0);
    }
    EXPECT_EQ(s.confidence, 0);

    thermal_fan_health_reset(&s);
    for (int i = 0; i < 8; i++) {           /* fault override */
        thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 1, 0);
    }
    EXPECT_EQ(s.confidence, 0);

    /* A skip mid-window resets the steady run. */
    thermal_fan_health_reset(&s);
    thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 0, 0);  /* run=1 */
    thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 0, 0);  /* run=2 */
    EXPECT_EQ(s.pwm_run, 2);
    thermal_fan_health_step(&s, &c, 120, 2400, 0, 0, 0, 0);  /* skip */
    EXPECT_EQ(s.have_ref, 0);
    EXPECT_EQ(s.pwm_run, 0);

    /* === Fault-override window also skips the trailing tick === */
    thermal_fan_health_reset(&s);
    thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 1, 0);  /* override */
    thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 0, 0);  /* trailing skip */
    EXPECT_EQ(s.have_ref, 0);
    thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 0, 0);  /* now sets ref */
    EXPECT_EQ(s.have_ref, 1);

    /* === Spin-up grace: the armed window skips its ticks === */
    thermal_fan_health_reset(&s);
    for (int i = 0; i < 5; i++) {           /* spinup_ticks = 5 */
        thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 0, 5);
        EXPECT_EQ(s.have_ref, 0);
    }
    thermal_fan_health_step(&s, &c, 120, 2400, 1, 0, 0, 5);  /* grace done */
    EXPECT_EQ(s.have_ref, 1);

    /* === Disabled config folds nothing === */
    c.enable = 0;
    thermal_fan_health_reset(&s);
    drive_point(&s, &c, 120, 1440, 8);
    EXPECT_EQ(s.confidence, 0);
    EXPECT_EQ(s.have_ref, 0);
}
