/* test/unit/test_fault_stale_context.c
 *
 * Unit tests for thermal_fault_stale_context_step. Simple timeout-based
 * FSM: condition active iff ms_since_last_valid >= context_timeout_ms.
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
    cfg->persist_ticks = 3;
    cfg->recovery_ticks = 2;
    cfg->correlated_context_id = 0xFFFF;
}

TEST_CASE(fault_stale_context) {
    thermal_fault_stale_context_state_t s;
    thermal_fault_detector_cfg_t cfg;
    make_cfg_basic(&cfg);
    uint32_t timeout = 3000;

    /* === Reset === */
    thermal_fault_stale_context_reset(&s);
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === Context fresh: stays NORMAL === */
    for (int t = 0; t < 10; t++) {
        thermal_fault_stale_context_step(&s, &cfg,
                                         /*ms_since_last_valid*/100, timeout,
                                         (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === Context stale (>= timeout) for < persist_ticks: still NORMAL === */
    thermal_fault_stale_context_reset(&s);
    for (int t = 0; t < 2; t++) {
        thermal_fault_stale_context_step(&s, &cfg, 5000, timeout,
                                         (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);
    EXPECT_EQ(s.persist_count, 2);

    /* Third consecutive tick -> DEGRADED (persist_ticks = 3) */
    thermal_fault_stale_context_step(&s, &cfg, 5000, timeout, 200);
    EXPECT_EQ(s.state, THERMAL_FAULT_DEGRADED);
    EXPECT_EQ(s.entered_ts_ms, 200);

    /* === Recovery: ms_since_last_valid < timeout for recovery_ticks -> NORMAL === */
    thermal_fault_stale_context_step(&s, &cfg, 100, timeout, 300);
    EXPECT_EQ(s.state, THERMAL_FAULT_RECOVERING);
    thermal_fault_stale_context_step(&s, &cfg, 100, timeout, 400);
    /* recovery_count = 1, recovery_ticks = 2 -> still recovering */
    EXPECT_EQ(s.state, THERMAL_FAULT_RECOVERING);
    thermal_fault_stale_context_step(&s, &cfg, 100, timeout, 500);
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === Exact-equality boundary: ms_since_last_valid == timeout is active === */
    thermal_fault_stale_context_reset(&s);
    for (int t = 0; t < 3; t++) {
        thermal_fault_stale_context_step(&s, &cfg, timeout, timeout,
                                         (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_DEGRADED);

    /* === enabled = 0 -> no-op === */
    thermal_fault_detector_cfg_t off_cfg;
    make_cfg_basic(&off_cfg);
    off_cfg.enabled = 0;
    thermal_fault_stale_context_reset(&s);
    for (int t = 0; t < 10; t++) {
        thermal_fault_stale_context_step(&s, &off_cfg, 99999, timeout,
                                         (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_NORMAL);

    /* === LATCHED via REQUEST_SHUTDOWN action === */
    thermal_fault_detector_cfg_t shut_cfg;
    make_cfg_basic(&shut_cfg);
    shut_cfg.action = THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN;
    thermal_fault_stale_context_reset(&s);
    for (int t = 0; t < 3; t++) {
        thermal_fault_stale_context_step(&s, &shut_cfg, 5000, timeout,
                                         (uint32_t)(t * 100));
    }
    EXPECT_EQ(s.state, THERMAL_FAULT_LATCHED);
    EXPECT_EQ(thermal_fault_stale_context_clear_allowed(&s, &shut_cfg), 0);

    /* Context freshness restored: recovery_count ticks toward clearable. */
    thermal_fault_stale_context_step(&s, &shut_cfg, 100, timeout, 300);
    EXPECT_EQ(s.recovery_count, 1);
    EXPECT_EQ(thermal_fault_stale_context_clear_allowed(&s, &shut_cfg), 0);
    thermal_fault_stale_context_step(&s, &shut_cfg, 100, timeout, 400);
    EXPECT_EQ(s.recovery_count, 2);
    EXPECT_EQ(thermal_fault_stale_context_clear_allowed(&s, &shut_cfg), 1);
}
