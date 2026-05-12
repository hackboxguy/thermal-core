/* core/thermal_pid.h
 *
 * Q16.16 PID governor per PRD §4.8 lines 668-676. Internal core header —
 * not included from core/thermal_core.h.
 *
 * Gain units (per-second convention):
 *   kp_q16: PWM / mc                     (proportional)
 *   ki_q16: PWM / (mc * s)               (integral, per-second)
 *   kd_q16: PWM * s / mc                 (derivative, per-second)
 *
 * Error = temp_mc - setpoint_mc; positive error -> more cooling.
 * Derivative is on measurement (temp - prev_temp), so setpoint changes
 * don't spike D. Anti-windup: integral is not updated when output is
 * saturated AND the increment would deepen saturation.
 *
 * The step function returns P/I/D terms in Q16.16 PWM for telemetry
 * visibility (TSIG_PID_*_SOC); the caller divides by 65536 if it needs
 * raw PWM.
 */
#ifndef THERMAL_PID_H
#define THERMAL_PID_H

#include <stdint.h>

typedef struct {
    int32_t  integral_q16;   /* accumulated I term in Q16.16 PWM */
    int32_t  prev_temp_mc;   /* for derivative on measurement */
    uint32_t prev_now_ms;    /* for dt computation */
    uint8_t  initialized;    /* 1 once first sample has been seen */
} thermal_pid_state_t;

typedef struct {
    int32_t  output_pwm;       /* saturated PWM in [pwm_min, pwm_max] */
    int32_t  p_term_q16;       /* Q16.16 PWM (post-saturating cast) */
    int32_t  i_term_q16;       /* Q16.16 PWM (== state.integral_q16 after step) */
    int32_t  d_term_q16;       /* Q16.16 PWM */
    uint16_t effective_dt_ms;  /* dt after clamp */
    uint8_t  saturated_high;   /* 1 if output_pwm clamped to pwm_max */
    uint8_t  saturated_low;    /* 1 if output_pwm clamped to pwm_min */
} thermal_pid_step_result_t;

/* Reset to pre-init state. Used by CMD_SET_PID at Stage 8 to clear
 * integral and derivative history when gains change. */
void thermal_pid_reset(thermal_pid_state_t *state);

/* Advance PID state by one sample.
 *   kp_q16, ki_q16, kd_q16, setpoint_mc -- per-zone PID config.
 *   temp_mc                             -- current aggregated zone temp.
 *   now_ms                              -- tick timestamp from input snapshot.
 *   dt_min_ms, dt_max_ms                -- dt clamp bounds.
 *                                          Caller guarantees dt_min_ms > 0
 *                                          and dt_min_ms <= dt_max_ms via
 *                                          validate_config.
 *   pwm_min, pwm_max                    -- output saturation. Anti-windup
 *                                          uses these to decide whether to
 *                                          commit the integral update.
 *   out                                 -- result struct, never NULL.
 *
 * Behavior on first call (state->initialized == 0):
 *   - dt = dt_min_ms (no prev_now_ms to compute from)
 *   - D = 0 (no prev_temp_mc)
 *   - I increment = ki_q16 * error * dt_min_ms / 1000
 *   - state->initialized set to 1 before return */
void thermal_pid_step(thermal_pid_state_t *state,
                      int32_t kp_q16, int32_t ki_q16, int32_t kd_q16,
                      int32_t setpoint_mc, int32_t temp_mc,
                      uint32_t now_ms,
                      uint16_t dt_min_ms, uint16_t dt_max_ms,
                      uint8_t pwm_min, uint8_t pwm_max,
                      thermal_pid_step_result_t *out);

#endif /* THERMAL_PID_H */
