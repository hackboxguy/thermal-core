/* core/thermal_zone.c — sensor aggregation (PRD §4.7 + §5.3). */
#include "thermal_zone.h"

void thermal_zone_aggregate(thermal_aggregation_t mode,
                            const int32_t *filter_values,
                            const uint8_t *filter_valid,
                            const int32_t *weights_q16,
                            uint8_t count,
                            int32_t fallback_temp_mc,
                            thermal_zone_aggregate_result_t *out) {
    if (count == 0) {
        out->temp_mc = fallback_temp_mc;
        out->valid = 0;
        return;
    }

    switch (mode) {
    case THERMAL_AGG_MAX: {
        int32_t best = 0;
        uint8_t any = 0;
        for (uint8_t i = 0; i < count; i++) {
            if (!filter_valid[i]) continue;
            if (!any || filter_values[i] > best) {
                best = filter_values[i];
            }
            any = 1;
        }
        if (!any) {
            out->temp_mc = fallback_temp_mc;
            out->valid = 0;
        } else {
            out->temp_mc = best;
            out->valid = 1;
        }
        return;
    }

    case THERMAL_AGG_AVG: {
        int64_t sum = 0;
        uint8_t n = 0;
        for (uint8_t i = 0; i < count; i++) {
            if (!filter_valid[i]) continue;
            sum += (int64_t)filter_values[i];
            n++;
        }
        if (n == 0) {
            out->temp_mc = fallback_temp_mc;
            out->valid = 0;
        } else {
            /* C99 signed-division truncation toward zero. */
            out->temp_mc = (int32_t)(sum / (int64_t)n);
            out->valid = 1;
        }
        return;
    }

    case THERMAL_AGG_WEIGHTED: {
        int64_t wsum = 0;        /* sum of filter[i] * weight_q16[i] over valid i */
        int64_t wtot = 0;        /* sum of weight_q16[i] over valid i */
        for (uint8_t i = 0; i < count; i++) {
            if (!filter_valid[i]) continue;
            wsum += (int64_t)filter_values[i] * (int64_t)weights_q16[i];
            wtot += (int64_t)weights_q16[i];
        }
        if (wtot == 0) {
            /* All valid sensors had zero weight, or no valid sensors at all.
             * Validation should prevent zero-weight-sum configs (PRD §5.3
             * line 800), but defensive for unit tests. */
            out->temp_mc = fallback_temp_mc;
            out->valid = 0;
        } else {
            out->temp_mc = (int32_t)(wsum / wtot);
            out->valid = 1;
        }
        return;
    }

    default:
        /* Unknown mode — defensive. Validation should reject this. */
        out->temp_mc = fallback_temp_mc;
        out->valid = 0;
        return;
    }
}
