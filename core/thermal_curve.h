/* core/thermal_curve.h
 *
 * Integer linear curve interpolation per PRD §4.8 line 677. Internal core
 * header — not included from core/thermal_core.h. Consumers: tests, the
 * acoustic-mask modifier (Stage 7), future curve-based policy.
 *
 * Contract:
 *   - `curve` must be sorted strictly ascending by x. Stage 4 validation
 *     enforces this; eval trusts the caller and does not re-check.
 *   - `count == 0` returns 0. `count == 1` returns that point's y.
 *   - For x <= curve[0].x, returns curve[0].value0/value1 (endpoint clamp).
 *   - For x >= curve[count-1].x, returns curve[count-1].value0/value1.
 *   - Within range, returns the linear interpolation between adjacent
 *     points using the PRD formula with int64 intermediate and C99
 *     truncation toward zero.
 */
#ifndef THERMAL_CURVE_H
#define THERMAL_CURVE_H

#include <stdint.h>
#include "thermal_types.h"

int32_t thermal_curve_eval_y0(const thermal_curve_point_t *curve,
                              uint8_t count, int32_t x);

int32_t thermal_curve_eval_y1(const thermal_curve_point_t *curve,
                              uint8_t count, int32_t x);

#endif /* THERMAL_CURVE_H */
