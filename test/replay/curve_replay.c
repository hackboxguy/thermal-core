/* test/replay/curve_replay.c
 *
 * Stage 2 replay driver. Sweeps speed 0..200 km/h in 1 km/h steps
 * through the canonical acoustic_mask curve and emits CSV to stdout.
 * Output is diffed against test/replay/golden/curve_sweep.csv in CI.
 *
 * The same curve is hardcoded in test/reference/curve.py with explicit
 * C99 truncation semantics. C and Python output must match byte-for-byte.
 */
#include <stdio.h>
#include <stdint.h>
#include "thermal_curve.h"
#include "thermal_types.h"

/* Canonical acoustic_mask curve from PRD §5.1 lines 729-734. */
static const thermal_curve_point_t CURVE[] = {
    {   0, 120,     0 },
    {  30, 180,     0 },
    {  80, 255, -5000 },
    { 130, 255, -8000 },
};
#define CURVE_COUNT ((uint8_t)(sizeof(CURVE) / sizeof(CURVE[0])))

int main(void) {
    printf("x_kmh,y0_pwm_cap,y1_trip_offset_mc\n");
    for (int32_t x = 0; x <= 200; x++) {
        int32_t y0 = thermal_curve_eval_y0(CURVE, CURVE_COUNT, x);
        int32_t y1 = thermal_curve_eval_y1(CURVE, CURVE_COUNT, x);
        printf("%d,%d,%d\n", (int)x, (int)y0, (int)y1);
    }
    return 0;
}
