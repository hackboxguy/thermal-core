/* core/thermal_slew.h
 *
 * Slew-rate limiter for actuator output (PRD §4.6 step 10 + safety
 * ordering at line 598). Pure function -- caller maintains prev_duty.
 *
 * safety_override_up == 1 exempts the upward step from slew. Downward
 * transitions always obey slew_per_tick, even when the flag is set
 * (a recovering CRITICAL/LATCHED path is not licensed to dump fan).
 *
 * slew_per_tick == 0 means no limit (request passes through).
 */
#ifndef THERMAL_SLEW_H
#define THERMAL_SLEW_H

#include <stdint.h>

typedef struct {
    uint8_t new_duty;
    uint8_t slew_limited;
} thermal_slew_result_t;

void thermal_slew_step(uint8_t prev_duty,
                       uint8_t requested_duty,
                       uint8_t slew_per_tick,
                       uint8_t safety_override_up,
                       thermal_slew_result_t *out);

#endif /* THERMAL_SLEW_H */
