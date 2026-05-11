/* test/unit/test_types_compile.c
 *
 * Stage 1 compile-time + size-budget test. References every public type
 * via sizeof so the compiler must see each full definition, and asserts
 * the three named size budgets stay within their compile-time ceilings.
 *
 * If a real struct's sizeof grows past its budget, the failure surfaces
 * here rather than at link-time several stages later. Bumping a budget
 * is a deliberate PR with rationale in the commit body.
 */
#include "harness.h"
#include "thermal_core.h"        /* transitively includes thermal_types.h,
                                    thermal_commands.h, thermal_platform.h,
                                    thermal_config.h */
#include "thermal_signals.h"
#include "thermal_events.h"

#define THERMAL_TEST_STATE_SNAPSHOT_BUDGET_BYTES   512
#define THERMAL_TEST_CONFIG_BUDGET_BYTES          2560
#define THERMAL_TEST_CORE_T_BUDGET_BYTES          THERMAL_CORE_T_RESERVED_BYTES

TEST_CASE(types_compile_and_fit_budgets) {
    /* === Reference every public typedef once. ===
     * sizeof forces the compiler to see the full definition; any
     * missing-field regression in headers above surfaces here. */

    /* thermal_types.h — enums */
    (void)sizeof(thermal_status_t);
    (void)sizeof(thermal_sample_kind_t);
    (void)sizeof(thermal_governor_t);
    (void)sizeof(thermal_aggregation_t);
    (void)sizeof(thermal_trip_severity_t);
    (void)sizeof(thermal_context_unit_t);
    (void)sizeof(thermal_context_failsafe_t);
    (void)sizeof(thermal_modifier_stage_t);
    (void)sizeof(thermal_fault_severity_t);
    (void)sizeof(thermal_fault_action_t);
    (void)sizeof(thermal_fault_state_t);
    (void)sizeof(thermal_fault_type_t);
    (void)sizeof(thermal_state_flags_t);
    (void)sizeof(thermal_actuator_reason_t);

    /* thermal_types.h — snapshot, output, state, curve */
    (void)sizeof(thermal_sample_t);
    (void)sizeof(thermal_input_snapshot_t);
    (void)sizeof(thermal_actuator_cmd_t);
    (void)sizeof(thermal_output_frame_t);
    (void)sizeof(thermal_zone_state_t);
    (void)sizeof(thermal_actuator_state_t);
    (void)sizeof(thermal_fault_state_snapshot_t);
    (void)sizeof(thermal_context_state_t);
    (void)sizeof(thermal_modifier_state_t);
    (void)sizeof(thermal_state_snapshot_t);
    (void)sizeof(thermal_curve_point_t);
    (void)sizeof(thermal_core_t);

    /* thermal_commands.h */
    (void)sizeof(thermal_command_id_t);
    (void)sizeof(thermal_command_t);
    (void)sizeof(thermal_command_result_t);

    /* thermal_platform.h */
    (void)sizeof(thermal_core_callbacks_t);

    /* thermal_signals.h, thermal_events.h */
    (void)sizeof(thermal_telemetry_signal_t);
    (void)sizeof(thermal_event_code_t);

    /* thermal_core.h — config sub-structs and master config */
    (void)sizeof(thermal_sensor_cfg_t);
    (void)sizeof(thermal_context_cfg_t);
    (void)sizeof(thermal_actuator_cfg_t);
    (void)sizeof(thermal_pid_cfg_t);
    (void)sizeof(thermal_trip_cfg_t);
    (void)sizeof(thermal_zone_cfg_t);
    (void)sizeof(thermal_modifier_cfg_t);
    (void)sizeof(thermal_fault_detector_cfg_t);
    (void)sizeof(thermal_fault_detection_cfg_t);
    (void)sizeof(thermal_telemetry_cfg_t);
    (void)sizeof(thermal_config_t);

    /* === Size budgets. Bumping a budget is a deliberate PR. === */
    EXPECT_LE(sizeof(thermal_state_snapshot_t),
              THERMAL_TEST_STATE_SNAPSHOT_BUDGET_BYTES);
    EXPECT_LE(sizeof(thermal_config_t),
              THERMAL_TEST_CONFIG_BUDGET_BYTES);
    EXPECT_LE(sizeof(thermal_core_t),
              THERMAL_TEST_CORE_T_BUDGET_BYTES);

    /* THERMAL_OK is the canonical zero-status sentinel; downstream code
     * commonly tests `if (status != THERMAL_OK)` and `if (!status)`. */
    EXPECT_EQ(THERMAL_OK, 0);
}
