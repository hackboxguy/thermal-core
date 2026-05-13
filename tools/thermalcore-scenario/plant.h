/* tools/thermalcore-scenario/plant.h
 *
 * Stage 12 12a — deterministic thermal-plant simulator.
 *
 * Per PRD §9.3 a first-order plant per zone with a configurable
 * heat-injection, ambient drift, fan-driven cooling, and
 * (optional) zone-to-zone coupling.  Q16.16 everywhere; no
 * floating point, no <math.h>, no syscalls.  Used by:
 *
 *   - tools/thermalcore-scenario/run.py (12b, via ctypes)
 *   - test/unit/test_plant.c (12a)
 *   - test/replay/ (optional reuse)
 *
 * The plant is the *single source of truth* for synthetic
 * temperature evolution in v1.  The scenario runner advances
 * this plant one tick per daemon TICK, then writes the resulting
 * temp_mc into the daemon's bsp_mock_tmpfs sensor file so the
 * daemon ticks against the simulated value.  The daemon's
 * actuator-write feeds back as the next plant_step's PWM input.
 *
 * Bit-for-bit determinism contract (PRD §9.3 line 1108):
 *   Same (config, scenario, git SHA) -> identical temp_mc
 *   sequence on every run, regardless of host CPU / compiler /
 *   build flags.  Achieved by Q16.16 arithmetic + explicit
 *   PRNG seed + pre-tick coupling snapshot.
 */
#ifndef THERMAL_PLANT_H
#define THERMAL_PLANT_H

#include <stdint.h>

#include "thermal_types.h"   /* thermal_curve_point_t */

#ifdef __cplusplus
extern "C" {
#endif

#define PLANT_MAX_ZONES        4
#define PLANT_MAX_FAN_POINTS   8

/* === Per-zone state + config =============================== */

typedef struct {
    /* === Dynamic state === */
    int32_t  temp_mc;              /* current temperature, millicelsius */

    /* === Static config (set via plant_zone_set_*) === */
    int32_t  ambient_mc;           /* anchor for Newton's-law cooling   */
    int32_t  load_w_q16;           /* heat injection, Q16.16 watts      */
    int32_t  heat_capacity_q16;    /* J/K (Q16.16); used to scale load  */
    int32_t  ambient_drift_q16;    /* (mc/s) per mc of (ambient - temp) */
    int32_t  fan_max_q16;          /* (mc/s) per mc of (temp - ambient)
                                      at fan_curve_eval == 1.0          */
    thermal_curve_point_t fan_curve[PLANT_MAX_FAN_POINTS];
    uint8_t  fan_curve_count;
    int8_t   coupling_neighbor;    /* -1 = no coupling                  */
    int32_t  coupling_q16;         /* (mc/s) per mc of (neighbor-this)  */
} plant_zone_t;

/* === Plant state =========================================== */

typedef struct {
    plant_zone_t zones[PLANT_MAX_ZONES];
    uint8_t      zone_count;

    /* PRNG state for noise injection.  32-bit LCG matching the
     * pattern in test/property/property_config.c. */
    uint32_t     prng_state;
    int32_t      noise_amplitude_mc;
} plant_t;

/* === Plant inputs (one PWM byte per zone) ================== */

typedef struct {
    uint8_t pwm[PLANT_MAX_ZONES];
} plant_inputs_t;

/* === Setup =============================================== */

/* Zero-initialise plant state.  After this call:
 *   - all zones have temp = ambient = 0, no coupling, no fan curve,
 *     load = 0, ambient_drift = fan_max = 0.
 *   - PRNG seeded with prng_seed; noise_amplitude = 0.
 *   - zone_count = 0 (caller must set via plant_set_zone_count). */
void plant_init(plant_t *p, uint32_t prng_seed);

void plant_set_zone_count(plant_t *p, uint8_t n);

/* Per-zone configuration. Each setter is a no-op if zone >=
 * zone_count or if a pointer arg is NULL. */
void plant_zone_set_temperature   (plant_t *p, uint8_t zone, int32_t temp_mc);
void plant_zone_set_ambient       (plant_t *p, uint8_t zone, int32_t ambient_mc);
void plant_zone_set_load_w_q16    (plant_t *p, uint8_t zone, int32_t load_w_q16);
void plant_zone_set_heat_capacity (plant_t *p, uint8_t zone, int32_t cap_q16);
void plant_zone_set_ambient_drift (plant_t *p, uint8_t zone, int32_t drift_q16);
void plant_zone_set_fan_max       (plant_t *p, uint8_t zone, int32_t fan_max_q16);
void plant_zone_set_fan_curve     (plant_t *p, uint8_t zone,
                                    const thermal_curve_point_t *points,
                                    uint8_t count);
void plant_zone_set_coupling      (plant_t *p, uint8_t zone,
                                    int8_t neighbor,
                                    int32_t coupling_q16);

void plant_set_noise_amplitude_mc (plant_t *p, int32_t amp_mc);

/* === Tick =============================================== */

/* Advance every zone by `dt_ms` milliseconds.  `inputs->pwm[z]`
 * is the actuator command for zone `z` (0..255), mirroring
 * `thermal_actuator_cmd_t.duty_0_255` from the daemon.
 *
 * Multi-zone coupling reads the **pre-tick** neighbor temperature
 * snapshot so the result is order-independent.  Per PRD §9.3,
 * `dt_ms` is the scenario clock advance; v1 callers typically
 * pass 100 (matching control_period_ms).
 *
 * No return value: failures are impossible at this layer (all
 * arithmetic uses int64 intermediates and the divisor is
 * compile-time non-zero). */
void plant_step(plant_t *p, const plant_inputs_t *inputs, uint32_t dt_ms);

/* === Reads ============================================== */

int32_t plant_zone_temp_mc       (const plant_t *p, uint8_t zone);
int32_t plant_zone_load_w_q16    (const plant_t *p, uint8_t zone);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_PLANT_H */
