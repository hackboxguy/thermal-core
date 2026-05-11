/* core/thermal_curve.c — integer linear interpolation per PRD §4.8. */
#include "thermal_curve.h"

/* Shared implementation. axis = 0 selects value0, axis = 1 selects value1. */
static int32_t curve_eval_axis(const thermal_curve_point_t *curve,
                               uint8_t count, int32_t x, int axis) {
    if (count == 0) {
        return 0;
    }
    #define Y(p) ((axis == 0) ? (p).value0 : (p).value1)

    if (x <= curve[0].x) {
        return Y(curve[0]);
    }
    if (x >= curve[count - 1].x) {
        return Y(curve[count - 1]);
    }

    /* Linear scan; count <= THERMAL_MAX_CURVE_POINTS = 8. Loop ends with
     * curve[i].x < x <= curve[i+1].x (strict on the low side because the
     * x == curve[0].x case is already handled by the clamp above). */
    uint8_t i = 0;
    while (i + 1 < count && x > curve[i + 1].x) {
        i++;
    }

    int32_t x0 = curve[i].x;
    int32_t x1 = curve[i + 1].x;
    int32_t y0 = Y(curve[i]);
    int32_t y1 = Y(curve[i + 1]);

    /* PRD §4.8: y = y0 + ((int64_t)(x - x0) * (y1 - y0)) / (x1 - x0).
     * int64 intermediate; C99 `/` is truncation toward zero. */
    int64_t num = (int64_t)(x - x0) * (int64_t)(y1 - y0);
    int64_t den = (int64_t)(x1 - x0);
    int32_t delta = (int32_t)(num / den);

    return y0 + delta;

    #undef Y
}

int32_t thermal_curve_eval_y0(const thermal_curve_point_t *curve,
                              uint8_t count, int32_t x) {
    return curve_eval_axis(curve, count, x, 0);
}

int32_t thermal_curve_eval_y1(const thermal_curve_point_t *curve,
                              uint8_t count, int32_t x) {
    return curve_eval_axis(curve, count, x, 1);
}
