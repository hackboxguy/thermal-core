/* test/unit/test_fault_stall.c
 *
 * Unit tests for thermal_fault_stall_step. Covers lifecycle, spin-up
 * grace, persist_ticks entry, recovery, and LATCHED behavior with
 * clear_allowed gating.
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
    cfg->severity = THERMAL_FAULT_SEVERITY_DEGRADED;
    cfg->action = THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED;
    cfg->persist_ticks = 5;
    cfg->recovery_ticks = 3;
    cfg->threshold0 = 200;   /* stall_rpm */
    cfg->threshold1 = 80;    /* stall_pwm_threshold */
    cfg->correlated_context_id = 0xFFFF;
}

TEST_CASE(fault_stall) {
    thermal_fault_stall_state_t s;
    thermal_fault_detector_cfg_t cfg;
    make_cfg_basic(&cfg);

    /* === Reset === */
    thermal_fault_stall_reset(&s);
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.persist_count, 0);
    EXPECT_EQ(s.recovery_count, 0);

    /* === Below PWM threshold: stays NORMAL even with low tach === */
    for (int t = 0; t < 10; t++) {
        thermal_fault_stall_step(&s, &cfg,
                                 /*req_pwm*/50, /*tach*/0, /*valid*/1,
                                 /*spinup_pwm*/100, /*spinup_ticks*/0,
                                 (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === Above PWM threshold with low tach for < persist: still NORMAL ===
     * spinup_ticks = 0 -> the 0->non-zero transition arms 0 ticks of
     * grace, so the very first step is already past spinup. */
    thermal_fault_stall_reset(&s);
    /* 4 ticks of stall condition (< persist=5) -> state stays NORMAL. */
    for (int t = 0; t < 4; t++) {
        thermal_fault_stall_step(&s, &cfg, 100, 0, 1, 100, 0,
                                 (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.persist_count, 4);

    /* === Fifth consecutive tick of stall condition: transition to DEGRADED ===
     * (severity = DEGRADED, action = FORCE_PWM_MAX_UNTIL_RECOVERED -> DEGRADED) */
    thermal_fault_stall_step(&s, &cfg, 100, 0, 1, 100, 0, 400);
    EXPECT_EQ(s.state, THERMAL_FAULT_DEGRADED);
    EXPECT_EQ(s.entered_ts_ms, 400);

    /* === Recovery: tach above threshold for recovery_ticks -> NORMAL === */
    thermal_fault_stall_step(&s, &cfg, 100, 500, 1, 100, 0, 600);
    EXPECT_EQ(s.state, THERMAL_FAULT_RECOVERING);
    thermal_fault_stall_step(&s, &cfg, 100, 500, 1, 100, 0, 700);
    EXPECT_EQ(s.state, THERMAL_FAULT_RECOVERING);
    EXPECT_EQ(s.recovery_count, 1);
    thermal_fault_stall_step(&s, &cfg, 100, 500, 1, 100, 0, 800);
    EXPECT_EQ(s.recovery_count, 2);
    thermal_fault_stall_step(&s, &cfg, 100, 500, 1, 100, 0, 900);
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === Spin-up grace: 0 -> non-zero arms countdown ===
     * spinup_ticks = 5. Stall condition during grace must NOT transition. */
    thermal_fault_stall_reset(&s);
    /* pwm=0 first */
    thermal_fault_stall_step(&s, &cfg, 0, 0, 1, 100, 5, 0);
    EXPECT_EQ(s.spinup_remaining, 0);
    /* 0 -> 100 transition arms 5-tick grace. */
    thermal_fault_stall_step(&s, &cfg, 100, 0, 1, 100, 5, 100);
    /* After arming, the countdown is decremented within the same step
     * AFTER the check. So we're "in spinup" for this tick. Confirm no
     * persist_count increment. */
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.persist_count, 0);
    /* Continue stalling during grace: still NORMAL. */
    for (int t = 2; t < 7; t++) {
        thermal_fault_stall_step(&s, &cfg, 100, 0, 1, 100, 5, (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.spinup_remaining, 0);
    /* After grace expires, persist_count starts accumulating. Need 5 more
     * ticks of stall to enter DEGRADED. */
    for (int t = 7; t < 12; t++) {
        thermal_fault_stall_step(&s, &cfg, 100, 0, 1, 100, 5, (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_DEGRADED);

    /* === tach_valid = 0 suppresses the condition ===
     * Per PRD: stall checks require valid tach. */
    thermal_fault_stall_reset(&s);
    for (int t = 0; t < 20; t++) {
        thermal_fault_stall_step(&s, &cfg, 100, 0, /*valid*/0, 100, 0,
                                 (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === LATCHED variant: action = FORCE_PWM_MAX_AND_LATCH === */
    thermal_fault_detector_cfg_t latch_cfg;
    make_cfg_basic(&latch_cfg);
    latch_cfg.action = THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH;
    thermal_fault_stall_reset(&s);
    for (int t = 0; t < 5; t++) {
        thermal_fault_stall_step(&s, &latch_cfg, 100, 0, 1, 100, 0,
                                 (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_LATCHED);

    /* clear_allowed: not yet (recovery_count = 0) */
    EXPECT_EQ(thermal_fault_stall_clear_allowed(&s, &latch_cfg), 0);

    /* Condition clears, recovery_count starts ticking. */
    for (int t = 5; t < 7; t++) {
        thermal_fault_stall_step(&s, &latch_cfg, 100, 500, 1, 100, 0,
                                 (uint32_t)(t * 100));
    }
    /* recovery_count = 2; recovery_ticks = 3; not yet clearable */
    EXPECT_EQ(thermal_fault_stall_clear_allowed(&s, &latch_cfg), 0);
    EXPECT_EQ(s.state, THERMAL_FAULT_LATCHED);  /* LATCHED stays */
    thermal_fault_stall_step(&s, &latch_cfg, 100, 500, 1, 100, 0, 700);
    /* recovery_count = 3; now clearable */
    EXPECT_EQ(thermal_fault_stall_clear_allowed(&s, &latch_cfg), 1);
    EXPECT_EQ(s.state, THERMAL_FAULT_LATCHED);  /* still LATCHED until external clear */

    /* === enabled = 0 means step is a no-op === */
    thermal_fault_detector_cfg_t off_cfg;
    make_cfg_basic(&off_cfg);
    off_cfg.enabled = 0;
    thermal_fault_stall_reset(&s);
    for (int t = 0; t < 20; t++) {
        thermal_fault_stall_step(&s, &off_cfg, 100, 0, 1, 100, 0,
                                 (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.persist_count, 0);
}
