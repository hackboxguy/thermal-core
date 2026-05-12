/* core/thermal_governor.h
 *
 * Step-wise governor: trip evaluation with hysteresis. Internal core
 * header — not included from core/thermal_core.h.
 *
 * Contract (PRD §4.8 line 667):
 *   "Step-wise governor: trip points are evaluated with hysteresis.
 *    Each active trip contributes a cooling_state; the zone request
 *    is the highest active state."
 *
 * Hysteresis (Schmitt trigger):
 *   - Trip becomes active when zone_temp_mc >= trip.temp_mc.
 *   - Trip becomes inactive when zone_temp_mc < trip.temp_mc - trip.hyst_mc.
 *   - In the dead band, the trip holds its previous-tick state from
 *     prev_active_mask.
 *
 * Validation guarantees (Stage 4 commit 3 / PRD §5.3 line 804):
 *   - Trip points are sorted by temperature ascending.
 *   - Hysteresis does not overlap neighboring trips
 *     (trip[i+1].temp_mc - trip[i+1].hyst_mc >= trip[i].temp_mc).
 * The eval function trusts these invariants.
 */
#ifndef THERMAL_GOVERNOR_H
#define THERMAL_GOVERNOR_H

#include <stdint.h>
#include "thermal_core.h"     /* for thermal_trip_cfg_t */

typedef struct {
    uint32_t active_trip_mask;  /* bit i set if trip i is currently active */
    uint8_t  cooling_state;     /* max cooling_state over active trips; 0 if none */
} thermal_governor_step_result_t;

/* Evaluate trips with hysteresis, return active mask and cooling_state.
 *   zone_temp_mc      — aggregated zone temperature (mc).
 *   trips             — array of len `trip_count`. May be NULL when trip_count == 0.
 *   trip_count        — 0..THERMAL_MAX_TRIPS_PER_ZONE.
 *   prev_active_mask  — previous-tick mask, for dead-band hold.
 *   out               — result struct, never NULL. */
void thermal_governor_step_wise(int32_t zone_temp_mc,
                                const thermal_trip_cfg_t *trips,
                                uint8_t trip_count,
                                uint32_t prev_active_mask,
                                thermal_governor_step_result_t *out);

#endif /* THERMAL_GOVERNOR_H */
