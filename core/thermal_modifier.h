/* core/thermal_modifier.h
 *
 * Policy modifier (acoustic_mask in v1) per PRD §4.5 + §5.1. Internal
 * core header. Wraps the curve interpolation to produce the per-tick
 * outputs the modifier applies:
 *
 *   pwm_cap         (from curve.value0)  -- post-governor PWM cap
 *   trip_offset_mc  (from curve.value1)  -- pre-governor setpoint/trip shift
 *
 * Fail-safe resolution (when the driving context is stale/invalid) is
 * the caller's responsibility: pass the post-fail-safe "effective"
 * context value. This module stays stateless.
 */
#ifndef THERMAL_MODIFIER_H
#define THERMAL_MODIFIER_H

#include <stdint.h>
#include "thermal_core.h"     /* for thermal_modifier_cfg_t */

typedef struct {
    int32_t pwm_cap;          /* curve.value0 evaluated at the context value */
    int32_t trip_offset_mc;   /* curve.value1 */
} thermal_modifier_eval_result_t;

void thermal_modifier_eval(const thermal_modifier_cfg_t *cfg,
                           int32_t effective_context_value,
                           thermal_modifier_eval_result_t *out);

#endif /* THERMAL_MODIFIER_H */
