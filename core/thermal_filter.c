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
     * KNOWN LIMITATION (impl-plan Stage 11d known limitations):
     * the `>> 16` arithmetic right-shift truncates toward negative
     * infinity (the gcc + clang interpretation of right-shift on
     * negative two's-complement ints).  Consequence: positive
     * deltas with |alpha_q16 * delta| < 65536 truncate to 0, so
     * positive-direction convergence asymptotes at
     * `target - ceil(65536/alpha_q16)` short of target -- e.g.,
     * 32 short for the canbus context's alpha_q16=2048.  Negative
     * deltas always produce at least -1 (floor), so decrease-
     * direction convergence reaches target exactly.  Round-to-
     * nearest would fix this symmetrically but rewrites the
     * SHA-256 of every replay golden + scenario CSV; the fix is
     * tracked work for the white-paper benchmark sweep.
     */
    int64_t delta = ((int64_t)alpha_q16 *
                     (int64_t)(sample - state->filtered_value)) >> 16;
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
