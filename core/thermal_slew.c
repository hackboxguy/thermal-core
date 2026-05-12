/* core/thermal_slew.c -- slew-rate limiter with override bypass. */
#include "thermal_slew.h"

void thermal_slew_step(uint8_t prev_duty,
                       uint8_t requested_duty,
                       uint8_t slew_per_tick,
                       uint8_t safety_override_up,
                       thermal_slew_result_t *out) {
    /* Upward safety override bypasses slew per PRD §4.6 line 598. Only
     * applies when the request is actually upward. */
    if (safety_override_up && requested_duty > prev_duty) {
        out->new_duty = requested_duty;
        out->slew_limited = 0;
        return;
    }

    if (slew_per_tick == 0) {
        out->new_duty = requested_duty;
        out->slew_limited = 0;
        return;
    }

    int16_t delta = (int16_t)requested_duty - (int16_t)prev_duty;
    if (delta > (int16_t)slew_per_tick) {
        out->new_duty = (uint8_t)(prev_duty + slew_per_tick);
        out->slew_limited = 1;
    } else if (delta < -(int16_t)slew_per_tick) {
        out->new_duty = (uint8_t)(prev_duty - slew_per_tick);
        out->slew_limited = 1;
    } else {
        out->new_duty = requested_duty;
        out->slew_limited = 0;
    }
}
