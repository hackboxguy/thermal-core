/* tools/thermalcore-scenario/plant.c
 *
 * Implementation of the deterministic thermal-plant simulator.
 * See plant.h for the contract.
 */
#include "plant.h"

#include <string.h>

#include "thermal_curve.h"

/* =============================================================== */
/* Q16.16 constants                                                 */
/* =============================================================== */
/* `Q16_ONE = 1 << 16` is defined by core/thermal_config.h (pulled
 * in via thermal_types.h -> thermal_config.h).  Reuse rather than
 * redeclare. */

/* Time-rate denominator: rates are Q16.16 (mc/s); to convert to a
 * raw mc delta over `dt_ms`, multiply by dt_ms (ms) then divide
 * by (1000 ms/s * Q16_ONE).  Both factors are compile-time so
 * the divisor is constant. */
#define Q16_PER_MS_DIVISOR  ((int64_t)Q16_ONE * 1000)

/* =============================================================== */
/* PRNG (LCG, parity with test/property/property_config.c)         */
/* =============================================================== */

static uint32_t plant_lcg(uint32_t *state)
{
    *state = (*state) * 1103515245u + 12345u;
    return *state;
}

/* =============================================================== */
/* Setup                                                            */
/* =============================================================== */

void plant_init(plant_t *p, uint32_t prng_seed)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->prng_state = prng_seed;
    /* Mark every zone's coupling as "none" by default. */
    for (uint8_t z = 0; z < PLANT_MAX_ZONES; z++) {
        p->zones[z].coupling_neighbor = -1;
    }
}

void plant_set_zone_count(plant_t *p, uint8_t n)
{
    if (!p) return;
    if (n > PLANT_MAX_ZONES) n = PLANT_MAX_ZONES;
    p->zone_count = n;
}

/* Bounds-check helper for the setters. */
static int valid_zone(const plant_t *p, uint8_t zone)
{
    return p && zone < p->zone_count;
}

void plant_zone_set_temperature(plant_t *p, uint8_t zone, int32_t temp_mc)
{
    if (!valid_zone(p, zone)) return;
    p->zones[zone].temp_mc = temp_mc;
}

void plant_zone_set_ambient(plant_t *p, uint8_t zone, int32_t ambient_mc)
{
    if (!valid_zone(p, zone)) return;
    p->zones[zone].ambient_mc = ambient_mc;
}

void plant_zone_set_load_w_q16(plant_t *p, uint8_t zone, int32_t load_w_q16)
{
    if (!valid_zone(p, zone)) return;
    p->zones[zone].load_w_q16 = load_w_q16;
}

void plant_zone_set_heat_capacity(plant_t *p, uint8_t zone, int32_t cap_q16)
{
    if (!valid_zone(p, zone)) return;
    /* Refuse a zero heat capacity to avoid divide-by-zero in the
     * tick math.  Treat as a no-op so the caller's existing
     * value stays valid. */
    if (cap_q16 == 0) return;
    p->zones[zone].heat_capacity_q16 = cap_q16;
}

void plant_zone_set_ambient_drift(plant_t *p, uint8_t zone, int32_t drift_q16)
{
    if (!valid_zone(p, zone)) return;
    p->zones[zone].ambient_drift_q16 = drift_q16;
}

void plant_zone_set_fan_max(plant_t *p, uint8_t zone, int32_t fan_max_q16)
{
    if (!valid_zone(p, zone)) return;
    p->zones[zone].fan_max_q16 = fan_max_q16;
}

void plant_zone_set_fan_curve(plant_t *p, uint8_t zone,
                              const thermal_curve_point_t *points,
                              uint8_t count)
{
    if (!valid_zone(p, zone) || !points) return;
    if (count > PLANT_MAX_FAN_POINTS) count = PLANT_MAX_FAN_POINTS;
    plant_zone_t *z = &p->zones[zone];
    for (uint8_t i = 0; i < count; i++) z->fan_curve[i] = points[i];
    z->fan_curve_count = count;
}

void plant_zone_set_coupling(plant_t *p, uint8_t zone,
                             int8_t neighbor, int32_t coupling_q16)
{
    if (!valid_zone(p, zone)) return;
    p->zones[zone].coupling_neighbor = neighbor;
    p->zones[zone].coupling_q16      = coupling_q16;
}

void plant_set_noise_amplitude_mc(plant_t *p, int32_t amp_mc)
{
    if (!p) return;
    if (amp_mc < 0) amp_mc = 0;
    p->noise_amplitude_mc = amp_mc;
}

/* =============================================================== */
/* Tick                                                             */
/* =============================================================== */

/* Convert a Q16.16 rate (mc/s) into a raw mc delta over dt_ms.
 * All arithmetic in int64 to avoid Q16.16 * 1000ms overflow. */
static int32_t rate_q16_to_delta_mc(int64_t rate_q16_per_s, uint32_t dt_ms)
{
    int64_t numerator = rate_q16_per_s * (int64_t)dt_ms;
    int64_t delta     = numerator / Q16_PER_MS_DIVISOR;
    if (delta > INT32_MAX) return INT32_MAX;
    if (delta < INT32_MIN) return INT32_MIN;
    return (int32_t)delta;
}

/* Tick one zone using a pre-snapshotted neighbor temperature. */
static int32_t step_one_zone(const plant_zone_t *z,
                             int32_t neighbor_pre_temp_mc,
                             uint8_t pwm,
                             uint32_t dt_ms)
{
    /* heat_in_q16: rate in mc/s, Q16.16. */
    int64_t heat_in_q16 = 0;

    /* Load injection: load (Q16.16 watts) / heat_capacity (Q16.16
     * J/K).  The Q16.16 cancellation keeps the result Q16.16 (mc/s
     * is the same units as K/s here, treating heat capacity as
     * having absorbed the unit conversion). */
    if (z->heat_capacity_q16 != 0) {
        heat_in_q16 += ((int64_t)z->load_w_q16 * Q16_ONE) /
                        z->heat_capacity_q16;
    }

    /* Newton's-law cooling toward ambient.  drift_q16 is (mc/s)
     * per mc, so the multiply with (ambient - temp) yields a
     * Q16.16 rate (mc/s).  Sign is naturally negative when temp >
     * ambient, i.e. heat *leaves* the zone (== negative
     * "heat_in"). */
    heat_in_q16 += (int64_t)z->ambient_drift_q16 *
                    (int64_t)(z->ambient_mc - z->temp_mc);

    /* Coupling: same Q16.16 rate shape. */
    if (z->coupling_neighbor >= 0) {
        heat_in_q16 += (int64_t)z->coupling_q16 *
                        (int64_t)(neighbor_pre_temp_mc - z->temp_mc);
    }

    /* Cooling via fan: curve_eval returns Q16.16 (0..65536); scale
     * by fan_max_q16 and (temp - ambient).  Cool_q16 has units of
     * (mc/s); subtracted from heat_in_q16. */
    int64_t cool_q16 = 0;
    if (z->fan_curve_count > 0 && z->fan_max_q16 != 0) {
        int32_t curve_y = thermal_curve_eval_y0(z->fan_curve,
                                                  z->fan_curve_count,
                                                  (int32_t)pwm);
        /* Clamp negative curve values to 0 (curves shouldn't go
         * negative but defend against malformed configs). */
        if (curve_y < 0) curve_y = 0;
        int64_t fan_scaled = ((int64_t)z->fan_max_q16 *
                               (int64_t)curve_y) / Q16_ONE;
        cool_q16 = fan_scaled * (int64_t)(z->temp_mc - z->ambient_mc);
    }

    int64_t net_rate_q16 = heat_in_q16 - cool_q16;
    return rate_q16_to_delta_mc(net_rate_q16, dt_ms);
}

void plant_step(plant_t *p, const plant_inputs_t *inputs, uint32_t dt_ms)
{
    if (!p || !inputs || dt_ms == 0) return;

    /* Pre-tick snapshot for coupling: PRD §9.3 determinism
     * requires the multi-zone update to be order-independent. */
    int32_t pre_temp[PLANT_MAX_ZONES];
    for (uint8_t z = 0; z < p->zone_count; z++) {
        pre_temp[z] = p->zones[z].temp_mc;
    }

    /* Advance each zone using the snapshotted neighbor temp. */
    for (uint8_t z = 0; z < p->zone_count; z++) {
        plant_zone_t *zone = &p->zones[z];
        int32_t neighbor_mc = 0;
        if (zone->coupling_neighbor >= 0 &&
            (uint8_t)zone->coupling_neighbor < p->zone_count) {
            neighbor_mc = pre_temp[zone->coupling_neighbor];
        }
        int32_t delta_mc = step_one_zone(zone, neighbor_mc,
                                          inputs->pwm[z], dt_ms);
        zone->temp_mc += delta_mc;
    }

    /* Optional noise (post-tick).  Uniform integer in
     * [-amp, +amp].  Single PRNG draw per zone keeps the test
     * "PRNG determinism with noise" trivially reproducible. */
    if (p->noise_amplitude_mc > 0) {
        uint32_t span = (uint32_t)(2 * p->noise_amplitude_mc + 1);
        for (uint8_t z = 0; z < p->zone_count; z++) {
            int32_t noise = (int32_t)(plant_lcg(&p->prng_state) % span)
                            - p->noise_amplitude_mc;
            p->zones[z].temp_mc += noise;
        }
    }
}

/* =============================================================== */
/* Reads                                                            */
/* =============================================================== */

int32_t plant_zone_temp_mc(const plant_t *p, uint8_t zone)
{
    if (!valid_zone(p, zone)) return 0;
    return p->zones[zone].temp_mc;
}

int32_t plant_zone_load_w_q16(const plant_t *p, uint8_t zone)
{
    if (!valid_zone(p, zone)) return 0;
    return p->zones[zone].load_w_q16;
}
