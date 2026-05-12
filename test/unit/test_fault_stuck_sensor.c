/* test/unit/test_fault_stuck_sensor.c
 *
 * Unit tests for thermal_fault_stuck_sensor_step. Covers window-based
 * detection, advisory mode (no correlated context), load-changing
 * gating, and varying-sensor negative case.
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
    cfg->action = THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK;
    cfg->persist_ticks = 1;       /* fire immediately after window check */
    cfg->recovery_ticks = 2;
    cfg->threshold0 = 100;        /* delta_mc */
    cfg->threshold1 = 10;         /* window_ticks */
    cfg->correlated_context_id = 7;  /* configured -> not advisory */
}

TEST_CASE(fault_stuck_sensor) {
    thermal_fault_stuck_sensor_state_t s;
    thermal_fault_detector_cfg_t cfg;
    make_cfg_basic(&cfg);

    /* === Reset === */
    thermal_fault_stuck_sensor_reset(&s);
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.window_tick_count, 0);

    /* === Constant sensor + load_changing for window_ticks -> DEGRADED ===
     * Sensor value 75000 mc constant; load_changing = 1 every tick;
     * window_ticks = 10; delta = 0 < delta_threshold = 100. */
    for (int t = 0; t < 10; t++) {
        thermal_fault_stuck_sensor_step(&s, &cfg,
                                        75000, /*valid*/1,
                                        /*load_changing*/1,
                                        (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_DEGRADED);

    /* === Constant sensor + NO load_changing: stays NORMAL ===
     * Stuck-sensor detector requires the load to be changing to call
     * the sensor "stuck" -- a constant reading on a constant load is
     * just normal idle behavior. */
    thermal_fault_stuck_sensor_reset(&s);
    for (int t = 0; t < 30; t++) {
        thermal_fault_stuck_sensor_step(&s, &cfg,
                                        75000, 1,
                                        /*load_changing*/0,
                                        (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === Varying sensor + load_changing: stays NORMAL ===
     * Sensor value swings beyond delta_threshold within the window. */
    thermal_fault_stuck_sensor_reset(&s);
    for (int t = 0; t < 30; t++) {
        int32_t v = 75000 + ((t & 1) ? 500 : -500);   /* +/- 500 mc swing */
        thermal_fault_stuck_sensor_step(&s, &cfg, v, 1, 1,
                                        (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === Advisory mode: correlated_context_id == 0xFFFF -> never fires ===
     * Even with constant sensor + load_changing, advisory mode suppresses
     * the condition. */
    thermal_fault_detector_cfg_t adv_cfg;
    make_cfg_basic(&adv_cfg);
    adv_cfg.correlated_context_id = 0xFFFF;
    thermal_fault_stuck_sensor_reset(&s);
    for (int t = 0; t < 30; t++) {
        thermal_fault_stuck_sensor_step(&s, &adv_cfg, 75000, 1, 1,
                                        (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === sensor_valid = 0 doesn't accumulate window === */
    thermal_fault_stuck_sensor_reset(&s);
    for (int t = 0; t < 30; t++) {
        thermal_fault_stuck_sensor_step(&s, &cfg, 75000, /*valid*/0, 1,
                                        (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.window_tick_count, 0);

    /* === enabled = 0 -> no-op === */
    thermal_fault_detector_cfg_t off_cfg;
    make_cfg_basic(&off_cfg);
    off_cfg.enabled = 0;
    thermal_fault_stuck_sensor_reset(&s);
    for (int t = 0; t < 30; t++) {
        thermal_fault_stuck_sensor_step(&s, &off_cfg, 75000, 1, 1,
                                        (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === LATCHED via FORCE_PWM_MAX_AND_LATCH action === */
    thermal_fault_detector_cfg_t latch_cfg;
    make_cfg_basic(&latch_cfg);
    latch_cfg.action = THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH;
    thermal_fault_stuck_sensor_reset(&s);
    for (int t = 0; t < 10; t++) {
        thermal_fault_stuck_sensor_step(&s, &latch_cfg, 75000, 1, 1,
                                        (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_LATCHED);
    EXPECT_EQ(thermal_fault_stuck_sensor_clear_allowed(&s, &latch_cfg), 0);
}
