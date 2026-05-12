/* test/unit/test_fault_runaway.c
 *
 * Unit tests for thermal_fault_runaway_step. Covers the four PRD §4.7
 * scenarios per plan §5 Stage 6 ("runaway formula"), plus reset,
 * window-fill gating, LATCHED behavior.
 */
#include <stdint.h>
#include <string.h>
#include "harness.h"
#include "thermal_fault.h"
#include "thermal_types.h"
#include "thermal_core.h"

static void make_cfg_basic(thermal_fault_detector_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 1;
    cfg->severity = THERMAL_FAULT_SEVERITY_CRITICAL;
    cfg->action = THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH;
    cfg->persist_ticks = 10;       /* window size */
    cfg->recovery_ticks = 5;
    cfg->threshold0 = 500;         /* rise_mc_threshold */
    cfg->threshold1 = 200;         /* cooling_pwm_threshold */
    cfg->correlated_context_id = 0xFFFF;
}

TEST_CASE(fault_runaway) {
    thermal_fault_runaway_state_t s;
    thermal_fault_detector_cfg_t cfg;
    make_cfg_basic(&cfg);

    /* === Reset === */
    thermal_fault_runaway_reset(&s);
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.window_head, 0);
    EXPECT_EQ(s.window_filled, 0);

    /* === Case 1: rising temp + high PWM for persist_ticks fires ===
     * temp ramps 80000 -> 81000 (delta = 1000 >= 500); pwm = 220 (>= 200). */
    for (int t = 0; t < 10; t++) {
        int32_t temp = 80000 + (int32_t)t * 100;
        thermal_fault_runaway_step(&s, &cfg, temp, 220, (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_LATCHED);  /* action = FORCE_PWM_MAX_AND_LATCH */

    /* === Case 2: rising temp + low PWM doesn't fire ===
     * Same temp ramp; pwm = 100 (< threshold = 200). */
    thermal_fault_runaway_reset(&s);
    for (int t = 0; t < 20; t++) {
        int32_t temp = 80000 + (int32_t)t * 100;
        thermal_fault_runaway_step(&s, &cfg, temp, 100, (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === Case 3: flat temp + high PWM doesn't fire ===
     * temp constant at 80000; pwm = 220. rise = 0 < 500. */
    thermal_fault_runaway_reset(&s);
    for (int t = 0; t < 20; t++) {
        thermal_fault_runaway_step(&s, &cfg, 80000, 220, (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === Case 4: oscillating PWM that dips below threshold doesn't fire ===
     * temp rising; pwm alternates 220 / 100. min(pwm) over window = 100 <
     * 200, so condition stays inactive even though temp is rising. */
    thermal_fault_runaway_reset(&s);
    for (int t = 0; t < 20; t++) {
        int32_t temp = 80000 + (int32_t)t * 100;
        uint8_t pwm = (t & 1) ? (uint8_t)100 : (uint8_t)220;
        thermal_fault_runaway_step(&s, &cfg, temp, pwm, (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === Window-fill gate: condition can't fire before window has
     *     persist_ticks samples. ===
     * With persist=10 and 9 samples of rising temp + high pwm, no fire. */
    thermal_fault_runaway_reset(&s);
    for (int t = 0; t < 9; t++) {
        int32_t temp = 80000 + (int32_t)t * 100;
        thermal_fault_runaway_step(&s, &cfg, temp, 220, (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.window_filled, 0);

    /* 10th sample fills window and fires. */
    thermal_fault_runaway_step(&s, &cfg, 81000, 220, 900);
    EXPECT_EQ(s.window_filled, 1);
    EXPECT_EQ(s.state, THERMAL_FAULT_LATCHED);

    /* === LATCHED clear_allowed gating ===
     * Once LATCHED, recovery_count ticks when condition inactive. */
    EXPECT_EQ(thermal_fault_runaway_clear_allowed(&s, &cfg), 0);

    /* Drive condition inactive: pwm low. */
    for (int t = 10; t < 14; t++) {
        thermal_fault_runaway_step(&s, &cfg, 81000, 100, (uint32_t)(t * 100));
    }
    /* recovery_count >= 4 but recovery_ticks = 5, not yet clearable. */
    EXPECT_EQ(thermal_fault_runaway_clear_allowed(&s, &cfg), 0);
    EXPECT_EQ(s.state, THERMAL_FAULT_LATCHED);
    thermal_fault_runaway_step(&s, &cfg, 81000, 100, 1400);
    EXPECT_EQ(thermal_fault_runaway_clear_allowed(&s, &cfg), 1);

    /* === enabled = 0 -> no-op === */
    thermal_fault_detector_cfg_t off_cfg;
    make_cfg_basic(&off_cfg);
    off_cfg.enabled = 0;
    thermal_fault_runaway_reset(&s);
    for (int t = 0; t < 20; t++) {
        int32_t temp = 80000 + (int32_t)t * 100;
        thermal_fault_runaway_step(&s, &off_cfg, temp, 220, (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.window_head, 0);   /* nothing pushed when disabled */
}
