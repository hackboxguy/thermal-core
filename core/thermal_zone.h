/* core/thermal_zone.h
 *
 * Sensor aggregation per PRD §4.7 lines 763-764 (partial-validity rules)
 * and PRD §5.3 line 800 (weighted-mode contract). Internal core header —
 * not included from core/thermal_core.h.
 *
 * Aggregation modes:
 *   THERMAL_AGG_MAX       — max of valid filter values
 *   THERMAL_AGG_AVG       — arithmetic mean of valid filter values
 *   THERMAL_AGG_WEIGHTED  — Q16.16-weighted mean of valid filter values
 *
 * All modes skip invalid sensors (filter_valid[i] == 0). If every sensor
 * is invalid (or weighted-mode has zero weight sum on valid sensors), the
 * result is fallback_temp_mc with valid = 0.
 */
#ifndef THERMAL_ZONE_H
#define THERMAL_ZONE_H

#include <stdint.h>
#include "thermal_types.h"     /* for thermal_aggregation_t */

typedef struct {
    int32_t temp_mc;  /* aggregated temperature; == fallback_temp_mc if valid == 0 */
    uint8_t valid;    /* 1 if at least one valid sensor contributed; 0 otherwise */
} thermal_zone_aggregate_result_t;

/* Aggregate filter values into a zone temperature.
 *   mode             — THERMAL_AGG_MAX / _AVG / _WEIGHTED.
 *   filter_values    — array of len `count`, the filtered_value per sensor.
 *   filter_valid     — array of len `count`, filter.valid per sensor.
 *   weights_q16      — array of len `count`, used only when mode is WEIGHTED;
 *                       may be NULL for MAX/AVG.
 *   count            — number of sensors. count == 0 returns the fallback.
 *   fallback_temp_mc — value to return when no valid sensors contribute.
 *   out              — result struct, never NULL. */
void thermal_zone_aggregate(thermal_aggregation_t mode,
                            const int32_t *filter_values,
                            const uint8_t *filter_valid,
                            const int32_t *weights_q16,
                            uint8_t count,
                            int32_t fallback_temp_mc,
                            thermal_zone_aggregate_result_t *out);

#endif /* THERMAL_ZONE_H */
