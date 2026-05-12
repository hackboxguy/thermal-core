/* core/thermal_modifier.c -- acoustic_mask curve wrapper (PRD §4.5). */
#include "thermal_modifier.h"
#include "thermal_curve.h"

void thermal_modifier_eval(const thermal_modifier_cfg_t *cfg,
                           int32_t effective_context_value,
                           thermal_modifier_eval_result_t *out) {
    out->pwm_cap = thermal_curve_eval_y0(cfg->curve, cfg->curve_count,
                                         effective_context_value);
    out->trip_offset_mc = thermal_curve_eval_y1(cfg->curve, cfg->curve_count,
                                                effective_context_value);
}
