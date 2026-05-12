/* core/thermal_governor.c — step-wise governor (PRD §4.8 line 667). */
#include "thermal_governor.h"

void thermal_governor_step_wise(int32_t zone_temp_mc,
                                const thermal_trip_cfg_t *trips,
                                uint8_t trip_count,
                                uint32_t prev_active_mask,
                                thermal_governor_step_result_t *out) {
    uint32_t new_mask = 0;
    uint8_t max_cs = 0;

    for (uint8_t i = 0; i < trip_count; i++) {
        const thermal_trip_cfg_t *t = &trips[i];
        uint32_t bit = (uint32_t)1u << i;
        uint8_t prev = (prev_active_mask & bit) ? 1u : 0u;
        uint8_t active;

        if (zone_temp_mc >= t->temp_mc) {
            active = 1u;
        } else if (zone_temp_mc < t->temp_mc - t->hyst_mc) {
            active = 0u;
        } else {
            /* Dead band: hold previous state. */
            active = prev;
        }

        if (active) {
            new_mask |= bit;
            if (t->cooling_state > max_cs) {
                max_cs = t->cooling_state;
            }
        }
    }

    out->active_trip_mask = new_mask;
    out->cooling_state = max_cs;
}
