/* core/thermal_filter.h
 *
 * Q16.16 first-order IIR low-pass + filter validity lifecycle.
 * Pinned by PRD §4.5 lines 554-575 and implementation plan §5 Stage 3.
 *
 * Math:
 *   filtered_next = prev + (int32_t)(((int64_t)alpha_q16 * (sample - prev)) >> 16)
 *
 * with int64 intermediate, arithmetic >> 16 (gcc/clang), and saturating
 * cast back to int32 ([INT32_MIN, INT32_MAX] clamp).
 *
 * Lifecycle:
 *   - Pre-init: valid = 0, initialized = 0, filtered_value undefined.
 *   - First-ever valid sample: filtered_value = sample (no IIR step from
 *     zero), valid = 1, initialized = 1.
 *   - Subsequent valid samples: IIR advances filtered_value; valid stays 1.
 *   - Invalid samples: no arithmetic update; filtered_value held;
 *     valid = 0.
 *   - Valid sample after an invalid run (initialized = 1): IIR resumes
 *     using the held filtered_value as prev (NOT re-initialized).
 */
#ifndef THERMAL_FILTER_H
#define THERMAL_FILTER_H

#include <stdint.h>

typedef struct {
    int32_t filtered_value;
    uint8_t valid;        /* current-tick: 1 if the most recent sample
                             was valid (numeric), 0 otherwise */
    uint8_t initialized;  /* lifetime: 1 once filtered_value has been
                             written at least once; never reset */
} thermal_filter_state_t;

/* Reset to pre-init state: filtered_value = 0, valid = 0, initialized = 0.
 * Callers that allocate state in static / zero-init memory can skip this. */
void thermal_filter_reset(thermal_filter_state_t *state);

/* Advance filter state by one sample.
 *   alpha_q16   — Q16.16 IIR coefficient (0..Q16_ONE in normal use).
 *   sample      — raw int32 sample (mc for temperature, RPM for tach, etc.).
 *   sample_valid — 1 if the sample is usable, 0 otherwise. */
void thermal_filter_step(thermal_filter_state_t *state,
                         int32_t alpha_q16,
                         int32_t sample,
                         uint8_t sample_valid);

#endif /* THERMAL_FILTER_H */
