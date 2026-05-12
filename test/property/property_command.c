/* test/property/property_command.c
 *
 * Property-test harness for thermal_core_apply_command (Stage 8 8b).
 * Builds one fixed valid config, inits a single thermal_core_t, then
 * runs N LCG-seeded random thermal_command_t values against the
 * persistent context. Emits CSV "seed,command_id,status,event_count"
 * to stdout.
 *
 * test/property/run_property_command.py invokes this binary and asserts:
 *   - every returned status is in {OK, INVALID_ARG, BOUNDS,
 *     REJECTED_SAFETY} (the documented apply_command set per PRD §7.5);
 *   - every call emits exactly one log_event (event_count == 1).
 *
 * Persistent state across seeds is intentional: the shadow cfg
 * accumulates random mutations, exercising "state machine survives N
 * arbitrary commands" as well as "each call returns a documented
 * status."
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "thermal_core.h"
#include "thermal_commands.h"
#include "thermal_config.h"
#include "thermal_types.h"

static uint32_t lcg_state;
static uint32_t lcg(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state;
}

static uint32_t mock_event_count_window;
static void mock_log_event(uint32_t ts, uint16_t code, uint32_t a1,
                           uint32_t a2, uint32_t a3, uint32_t a4) {
    (void)ts; (void)code; (void)a1; (void)a2; (void)a3; (void)a4;
    mock_event_count_window++;
}

/* Fixed valid cfg shape: 2 sensors, 1 actuator, 1 step-wise zone,
 * 1 PID zone with full bounds + CRITICAL floor, 1 acoustic_mask
 * modifier, 1 speed context, runaway detector enabled. Every command
 * kind has at least one plausible valid target. */
static void build_valid_cfg(thermal_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->config_version = 1;
    cfg->control_period_ms = 100;

    cfg->sensor_count = 2;
    cfg->sensors[0].id = 1;
    cfg->sensors[0].iir_alpha_q16 = Q16_ONE;
    cfg->sensors[0].max_staleness_ms = 1000;
    cfg->sensors[1].id = 2;
    cfg->sensors[1].iir_alpha_q16 = Q16_ONE;
    cfg->sensors[1].max_staleness_ms = 1000;

    cfg->context_count = 1;
    cfg->contexts[0].id = 100;
    cfg->contexts[0].unit = THERMAL_CONTEXT_UNIT_KMH;
    cfg->contexts[0].iir_alpha_q16 = Q16_ONE;
    cfg->contexts[0].timeout_ms = 500;
    cfg->contexts[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;

    cfg->actuator_count = 1;
    cfg->actuators[0].id = 10;
    cfg->actuators[0].pwm_min = 80;
    cfg->actuators[0].pwm_max = 255;
    cfg->actuators[0].slew_per_tick = 8;
    cfg->actuators[0].state_pwm[0] = 0;
    cfg->actuators[0].state_pwm[1] = 100;
    cfg->actuators[0].state_pwm[2] = 160;
    cfg->actuators[0].state_pwm[3] = 220;
    cfg->actuators[0].state_pwm[4] = 255;

    /* Zone 0: step-wise. */
    cfg->zones[0].sensor_count = 1;
    cfg->zones[0].sensor_ids[0] = 1;
    cfg->zones[0].aggregation = THERMAL_AGG_MAX;
    cfg->zones[0].governor = THERMAL_GOVERNOR_STEP_WISE;
    cfg->zones[0].actuator_count = 1;
    cfg->zones[0].actuator_ids[0] = 10;
    cfg->zones[0].fallback_temp_mc = 50000;
    cfg->zones[0].trip_count = 3;
    cfg->zones[0].trips[0].temp_mc = 70000;
    cfg->zones[0].trips[0].hyst_mc = 2000;
    cfg->zones[0].trips[0].severity = THERMAL_TRIP_WARN;
    cfg->zones[0].trips[0].cooling_state = 2;
    cfg->zones[0].trips[1].temp_mc = 85000;
    cfg->zones[0].trips[1].hyst_mc = 2000;
    cfg->zones[0].trips[1].severity = THERMAL_TRIP_CRITICAL;
    cfg->zones[0].trips[1].cooling_state = 4;
    cfg->zones[0].trips[2].temp_mc = 95000;
    cfg->zones[0].trips[2].hyst_mc = 2000;
    cfg->zones[0].trips[2].severity = THERMAL_TRIP_SHUTDOWN;
    cfg->zones[0].trips[2].cooling_state = 4;

    /* Zone 1: PID with full bounds + CRITICAL floor. */
    cfg->zones[1].sensor_count = 1;
    cfg->zones[1].sensor_ids[0] = 2;
    cfg->zones[1].aggregation = THERMAL_AGG_MAX;
    cfg->zones[1].governor = THERMAL_GOVERNOR_PID;
    cfg->zones[1].actuator_count = 1;
    cfg->zones[1].actuator_ids[0] = 10;
    cfg->zones[1].fallback_temp_mc = 50000;
    cfg->zones[1].pid.kp_q16 = 4915;
    cfg->zones[1].pid.ki_q16 = 327;
    cfg->zones[1].pid.kd_q16 = 0;
    cfg->zones[1].pid.setpoint_mc = 75000;
    cfg->zones[1].pid.kp_min_q16 = 0;
    cfg->zones[1].pid.kp_max_q16 = 327680;
    cfg->zones[1].pid.ki_min_q16 = 0;
    cfg->zones[1].pid.ki_max_q16 = 65536;
    cfg->zones[1].pid.kd_min_q16 = 0;
    cfg->zones[1].pid.kd_max_q16 = 65536;
    cfg->zones[1].pid.setpoint_min_mc = 50000;
    cfg->zones[1].pid.setpoint_max_mc = 95000;
    cfg->zones[1].pid.dt_min_ms = 50;
    cfg->zones[1].pid.dt_max_ms = 500;
    cfg->zones[1].trip_count = 2;
    cfg->zones[1].trips[0].temp_mc = 80000;
    cfg->zones[1].trips[0].hyst_mc = 2000;
    cfg->zones[1].trips[0].severity = THERMAL_TRIP_WARN;
    cfg->zones[1].trips[0].cooling_state = 2;
    cfg->zones[1].trips[1].temp_mc = 90000;
    cfg->zones[1].trips[1].hyst_mc = 2000;
    cfg->zones[1].trips[1].severity = THERMAL_TRIP_CRITICAL;
    cfg->zones[1].trips[1].cooling_state = 4;
    cfg->zone_count = 2;

    cfg->modifier_count = 1;
    strncpy(cfg->modifiers[0].name, "acoustic_mask", THERMAL_NAME_MAX - 1);
    cfg->modifiers[0].context_id = 100;
    cfg->modifiers[0].stages =
        THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET |
        THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP;
    cfg->modifiers[0].curve_count = 4;
    cfg->modifiers[0].curve[0].x = 0;
    cfg->modifiers[0].curve[0].value0 = 120;
    cfg->modifiers[0].curve[1].x = 30;
    cfg->modifiers[0].curve[1].value0 = 180;
    cfg->modifiers[0].curve[2].x = 80;
    cfg->modifiers[0].curve[2].value0 = 255;
    cfg->modifiers[0].curve[2].value1 = -5000;
    cfg->modifiers[0].curve[3].x = 130;
    cfg->modifiers[0].curve[3].value0 = 255;
    cfg->modifiers[0].curve[3].value1 = -8000;
    cfg->modifiers[0].fail_safe = THERMAL_FAILSAFE_ASSUME_STATIONARY;

    cfg->faults.runaway_defaults.enabled = 1;
    cfg->faults.runaway_defaults.severity = THERMAL_FAULT_SEVERITY_CRITICAL;
    cfg->faults.runaway_defaults.action =
        THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED;
    cfg->faults.runaway_defaults.persist_ticks = 5;
    cfg->faults.runaway_defaults.recovery_ticks = 5;
    cfg->faults.runaway_defaults.threshold0 = 1000;
    cfg->faults.runaway_defaults.threshold1 = 200;
}

/* 90% of seeds pick command_id from {1..5}, 10% pick a random uint16
 * (tests the unknown-command path). Union fields are filled with
 * random uint16/int32 -- mostly out-of-range, so most calls exercise
 * BOUNDS / INVALID_ARG / REJECTED_SAFETY paths. */
static void make_random_command(thermal_command_t *c) {
    memset(c, 0, sizeof(*c));
    uint32_t roll = lcg();
    if ((roll % 10u) == 0u) {
        c->command_id = (uint16_t)(lcg() & 0xFFFFu);   /* random, often unknown */
    } else {
        c->command_id = (uint16_t)(1u + (lcg() % 5u)); /* one of {1..5} */
    }
    switch (c->command_id) {
    case THERMAL_CMD_SET_PID:
        c->u.set_pid.zone_id = (uint16_t)(lcg() & 0x7u);  /* 0..7; zone_count=2 */
        c->u.set_pid.kp_q16  = (int32_t)lcg();
        c->u.set_pid.ki_q16  = (int32_t)lcg();
        c->u.set_pid.kd_q16  = (int32_t)lcg();
        break;
    case THERMAL_CMD_SET_SETPOINT:
        c->u.set_setpoint.zone_id     = (uint16_t)(lcg() & 0x7u);
        c->u.set_setpoint.setpoint_mc = (int32_t)lcg();
        break;
    case THERMAL_CMD_SET_TRIP:
        c->u.set_trip.zone_id  = (uint16_t)(lcg() & 0x7u);
        c->u.set_trip.trip_idx = (uint16_t)(lcg() & 0x7u);
        c->u.set_trip.temp_mc  = (int32_t)lcg();
        c->u.set_trip.hyst_mc  = (int32_t)lcg();
        break;
    case THERMAL_CMD_SET_CURVE_POINT:
        c->u.set_curve_point.modifier_id = (uint16_t)(lcg() & 0x7u);
        c->u.set_curve_point.point_idx   = (uint16_t)(lcg() & 0xFu);
        c->u.set_curve_point.x      = (int32_t)lcg();
        c->u.set_curve_point.value0 = (int32_t)lcg();
        c->u.set_curve_point.value1 = (int32_t)lcg();
        break;
    case THERMAL_CMD_CLEAR_FAULT:
        c->u.clear_fault.fault_type = (uint16_t)(lcg() & 0x7u);
        c->u.clear_fault.target_id  = (uint16_t)(lcg() & 0xFFu);
        break;
    default:
        /* Random payload bits -- apply_command should ignore them and
         * return INVALID_ARG on the unknown command_id. */
        (void)lcg(); (void)lcg(); (void)lcg();
        break;
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <n_seeds>\n", argv[0]);
        return 2;
    }
    int n_signed = atoi(argv[1]);
    if (n_signed <= 0) {
        fprintf(stderr, "n_seeds must be positive\n");
        return 2;
    }
    uint32_t n = (uint32_t)n_signed;

    thermal_config_t cfg;
    build_valid_cfg(&cfg);
    static thermal_core_t ctx;
    thermal_core_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.log_event = mock_log_event;
    if (thermal_core_init(&ctx, &cfg, &cb) != THERMAL_OK) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    printf("seed,command_id,status,event_count\n");
    for (uint32_t seed = 1; seed <= n; seed++) {
        lcg_state = seed;
        thermal_command_t cmd;
        make_random_command(&cmd);
        thermal_command_result_t r;
        mock_event_count_window = 0;
        thermal_status_t s = thermal_core_apply_command(&ctx, seed * 100u,
                                                       &cmd, &r);
        printf("%u,%u,%d,%u\n",
               seed, (unsigned)cmd.command_id, (int)s,
               (unsigned)mock_event_count_window);
    }
    return 0;
}
