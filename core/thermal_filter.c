/* core/thermal_filter.c — Q16.16 IIR + validity lifecycle (PRD §4.5). */
#include "thermal_filter.h"

void thermal_filter_reset(thermal_filter_state_t *state) {
    state->filtered_value = 0;
    state->valid = 0;
    state->initialized = 0;
}

void thermal_filter_step(thermal_filter_state_t *state,
                         int32_t alpha_q16,
                         int32_t sample,
                         uint8_t sample_valid) {
    if (!sample_valid) {
        state->valid = 0;
        /* filtered_value and initialized unchanged */
        return;
    }
    if (!state->initialized) {
        /* First-ever valid sample: initialize directly to sample. */
        state->filtered_value = sample;
        state->initialized = 1;
        state->valid = 1;
        return;
    }
    /* IIR step from held filtered_value (possibly after an invalid run).
     *
     * delta = round(alpha_q16 * (sample - filtered) / 2^16), rounded
     * half-away-from-zero.  A plain `>> 16` floors (gcc + clang
     * arithmetic shift), which would stall positive convergence
     * `ceil(2^16/alpha_q16)` short of target while letting negative
     * convergence reach target exactly -- the sign-aware rounding
     * keeps the two directions symmetric.  `-product` cannot
     * overflow: product is alpha_q16 (<= 2^16) times a millidegree
     * difference, far inside int64.
     */
    int64_t product = (int64_t)alpha_q16 *
                      (int64_t)(sample - state->filtered_value);
    int64_t delta = (product >= 0)
                  ?  ( (product + 32768) >> 16)
                  : -(((-product) + 32768) >> 16);
    int64_t next  = (int64_t)state->filtered_value + delta;
    if (next > INT32_MAX) {
        state->filtered_value = INT32_MAX;
    } else if (next < INT32_MIN) {
        state->filtered_value = INT32_MIN;
    } else {
        state->filtered_value = (int32_t)next;
    }
    state->valid = 1;
}
