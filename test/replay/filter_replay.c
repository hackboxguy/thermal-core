/* test/replay/filter_replay.c
 *
 * Stage 3 replay driver. Generates 1000 samples deterministically
 * (triangle wave + Knuth-hash jitter + injected invalid run idx 300..349),
 * runs them through thermal_filter_step at alpha_q16 = 16384, and emits a
 * 5-column CSV to stdout. The same generator and filter math live in
 * test/reference/iir.py; both must produce identical CSV output.
 */
#include <stdio.h>
#include <stdint.h>
#include "thermal_filter.h"

#define N_SAMPLES   1000
#define ALPHA_Q16   16384  /* 0.25 in Q16.16 */

/* Triangle wave 45000mc -> 90000mc -> 45000mc over 0..1000. */
static int32_t base_temp(uint32_t i) {
    if (i <= 500) return 45000 + (int32_t)(i * 90);
    return 90000 - (int32_t)((i - 500) * 90);
}

/* Per-index pseudo-jitter in [-256, +255] mc. Knuth multiplicative hash;
 * uint32 wraparound is well-defined. */
static int32_t jitter(uint32_t i) {
    uint32_t h = i * 2654435761u;
    return (int32_t)((h >> 16) & 0x1FFu) - 256;
}

/* Injected invalid run: 50 consecutive invalid samples at idx 300..349. */
static uint8_t sample_is_valid(uint32_t i) {
    return (i >= 300 && i < 350) ? 0u : 1u;
}

int main(void) {
    thermal_filter_state_t s = { 0, 0, 0 };
    printf("idx,sample,sample_valid,filtered,filter_valid\n");
    for (uint32_t i = 0; i < N_SAMPLES; i++) {
        int32_t sample = base_temp(i) + jitter(i);
        uint8_t valid  = sample_is_valid(i);
        thermal_filter_step(&s, ALPHA_Q16, sample, valid);
        printf("%u,%d,%u,%d,%u\n",
               (unsigned)i, sample, valid,
               s.filtered_value, s.valid);
    }
    return 0;
}
