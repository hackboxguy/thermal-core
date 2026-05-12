/* core/thermal_pid.c -- Q16.16 PID governor (PRD §4.8). */
#include <stdint.h>
#include "thermal_pid.h"

void thermal_pid_reset(thermal_pid_state_t *state) {
    state->integral_q16 = 0;
    state->prev_temp_mc = 0;
    state->prev_now_ms = 0;
    state->initialized = 0;
}

void thermal_pid_step(thermal_pid_state_t *state,
                      int32_t kp_q16, int32_t ki_q16, int32_t kd_q16,
                      int32_t setpoint_mc, int32_t temp_mc,
                      uint32_t now_ms,
                      uint16_t dt_min_ms, uint16_t dt_max_ms,
                      uint8_t pwm_min, uint8_t pwm_max,
                      thermal_pid_step_result_t *out) {
    /* dt: first call -> dt_min_ms; otherwise (now_ms - prev_now_ms) clamped. */
    uint32_t dt_ms;
    if (state->initialized) {
        dt_ms = (uint32_t)(now_ms - state->prev_now_ms);
    } else {
        dt_ms = (uint32_t)dt_min_ms;
    }
    if (dt_ms < (uint32_t)dt_min_ms) dt_ms = (uint32_t)dt_min_ms;
    if (dt_ms > (uint32_t)dt_max_ms) dt_ms = (uint32_t)dt_max_ms;

    /* Error: positive when zone is too hot. */
    int32_t error_mc = temp_mc - setpoint_mc;

    /* P term: kp_q16 * error_mc -> Q16.16 PWM. */
    int64_t p_term_q16 = (int64_t)kp_q16 * (int64_t)error_mc;

    /* Integral increment: ki_q16 * error * dt_ms / 1000 -> Q16.16 PWM. */
    int64_t i_increment =
        (int64_t)ki_q16 * (int64_t)error_mc * (int64_t)dt_ms / 1000;

    /* Provisional integral (int64, saturated to int32 range). */
    int64_t i_tentative = (int64_t)state->integral_q16 + i_increment;
    if (i_tentative > INT32_MAX) i_tentative = INT32_MAX;
    else if (i_tentative < INT32_MIN) i_tentative = INT32_MIN;

    /* D term: derivative on measurement.
     *   d_temp = temp - prev_temp (mc per dt_ms)
     *   rate (mc/s) = d_temp * 1000 / dt_ms
     *   D (Q16.16 PWM) = kd_q16 * d_temp * 1000 / dt_ms */
    int64_t d_term_q16 = 0;
    if (state->initialized) {
        int32_t d_temp = temp_mc - state->prev_temp_mc;
        d_term_q16 = (int64_t)kd_q16 * (int64_t)d_temp * 1000 / (int64_t)dt_ms;
    }

    /* Sum in Q16.16 PWM, saturate to int32 before shifting back. */
    int64_t output_q16 = p_term_q16 + i_tentative + d_term_q16;
    if (output_q16 > INT32_MAX) output_q16 = INT32_MAX;
    else if (output_q16 < INT32_MIN) output_q16 = INT32_MIN;

    /* Q16.16 -> raw PWM via arithmetic >> 16. */
    int32_t output_pwm = (int32_t)(output_q16 >> 16);

    /* PWM saturation to configured bounds. */
    uint8_t sat_hi = 0, sat_lo = 0;
    if (output_pwm > (int32_t)pwm_max) { output_pwm = (int32_t)pwm_max; sat_hi = 1; }
    if (output_pwm < (int32_t)pwm_min) { output_pwm = (int32_t)pwm_min; sat_lo = 1; }

    /* Anti-windup: skip integral update if saturated AND increment deepens
     * saturation. Releasing-direction updates always commit. */
    int commit_integral = 1;
    if (sat_hi && i_increment > 0) commit_integral = 0;
    if (sat_lo && i_increment < 0) commit_integral = 0;
    if (commit_integral) {
        state->integral_q16 = (int32_t)i_tentative;
    }

    /* Commit other state. */
    state->prev_temp_mc = temp_mc;
    state->prev_now_ms = now_ms;
    state->initialized = 1;

    /* Populate result (Q16.16 fields saturated to int32 for telemetry). */
    out->output_pwm = output_pwm;
    out->p_term_q16 = (int32_t)((p_term_q16 > INT32_MAX) ? INT32_MAX
                              : (p_term_q16 < INT32_MIN) ? INT32_MIN
                              : p_term_q16);
    out->i_term_q16 = state->integral_q16;
    out->d_term_q16 = (int32_t)((d_term_q16 > INT32_MAX) ? INT32_MAX
                              : (d_term_q16 < INT32_MIN) ? INT32_MIN
                              : d_term_q16);
    out->effective_dt_ms = (uint16_t)dt_ms;
    out->saturated_high = sat_hi;
    out->saturated_low = sat_lo;
}
