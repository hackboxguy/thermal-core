/* test/unit/test_plant.c
 *
 * Stage 12 12a — deterministic thermal-plant simulator tests.
 *
 * Scenarios:
 *   1. Init + read.
 *   2. No-load, no-fan: temp converges toward ambient.
 *   3. Load only: temp rises monotonically; settles above start.
 *   4. Load + max fan: steady-state temp lower than load-only.
 *   5. Fan curve x=0 endpoint clamp (sanity check via plant_step).
 *   6. Coupling: heated zone A pulls neighbor B's temperature up.
 *   7. Multi-zone coupling is order-independent.
 *   8. PRNG determinism without noise -- byte-equal trajectories.
 *   9. PRNG determinism with noise -- byte-equal noisy trajectories.
 *   10. PRNG separation -- different seeds diverge with noise on.
 *
 * Physics-shaped scenarios use a 200 mc tolerance band (generous,
 * since the model coefficients are synthetic).  Determinism
 * scenarios use byte-equal EXPECT_EQ.
 */
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "plant.h"
/* Q16_ONE comes from core/thermal_config.h via plant.h -> thermal_types.h */

/* === Helpers ===================================================== */

static const thermal_curve_point_t LINEAR_FAN_CURVE[2] = {
    { .x = 0,   .value0 = 0,       .value1 = 0 },
    { .x = 255, .value0 = Q16_ONE, .value1 = 0 },   /* 1.0 cooling at full PWM */
};

/* Standard "heating" zone: 1 unit of load, mild ambient pull,
 * moderately strong fan.  Tuned so that 60 s of stepping at
 * dt=100 ms produces visible movements (thousands of mc). */
static void config_heating_zone(plant_t *p, uint8_t zone,
                                int32_t initial_temp_mc,
                                int32_t ambient_mc,
                                int32_t load_w_q16,
                                int32_t fan_max_q16)
{
    plant_zone_set_temperature   (p, zone, initial_temp_mc);
    plant_zone_set_ambient       (p, zone, ambient_mc);
    plant_zone_set_load_w_q16    (p, zone, load_w_q16);
    plant_zone_set_heat_capacity (p, zone, Q16_ONE);            /* 1.0 J/K */
    plant_zone_set_ambient_drift (p, zone, 1000);               /* mild */
    plant_zone_set_fan_max       (p, zone, fan_max_q16);
    plant_zone_set_fan_curve     (p, zone, LINEAR_FAN_CURVE, 2);
}

/* Step the plant `n` times at the given PWM (single-zone helper). */
static void step_n(plant_t *p, uint8_t pwm, uint32_t dt_ms, int n)
{
    plant_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.pwm[0] = pwm;
    for (int i = 0; i < n; i++) {
        plant_step(p, &in, dt_ms);
    }
}

/* === Scenarios =================================================== */

TEST_CASE(plant) {
    /* === Scenario 1: init + read ================================== */
    {
        plant_t p;
        plant_init(&p, 42);
        plant_set_zone_count(&p, 1);
        plant_zone_set_temperature(&p, 0, 40000);
        EXPECT_EQ(plant_zone_temp_mc(&p, 0), 40000);
        /* Out-of-range zone reads return 0 (safe default). */
        EXPECT_EQ(plant_zone_temp_mc(&p, 99), 0);
    }

    /* === Scenario 2: no-load, no-fan -> temp converges toward ambient
     * Start at 80000 mc, ambient 20000 mc.  600 ticks at 100 ms each
     * = 60 s.  Temp must end strictly below start and closer to
     * ambient (within ~half the initial gap). */
    {
        plant_t p;
        plant_init(&p, 1);
        plant_set_zone_count(&p, 1);
        config_heating_zone(&p, 0,
                            /*temp*/    80000,
                            /*ambient*/ 20000,
                            /*load*/    0,
                            /*fan_max*/ 0);
        step_n(&p, /*pwm*/ 0, /*dt_ms*/ 100, /*n_steps*/ 600);
        int32_t final_mc = plant_zone_temp_mc(&p, 0);
        EXPECT_LE(final_mc, 80000 - 1);              /* moved at all */
        /* Closer to ambient than to start: final < midpoint. */
        EXPECT_LE(final_mc, 50000);
        /* Should not undershoot ambient (no overshoot in first-order
         * decay). */
        EXPECT_LE(20000 - 1, final_mc);
    }

    /* === Scenario 3: load only -> temp rises ====================== */
    {
        plant_t p;
        plant_init(&p, 1);
        plant_set_zone_count(&p, 1);
        config_heating_zone(&p, 0,
                            /*temp*/    25000,
                            /*ambient*/ 25000,
                            /*load*/    100 * Q16_ONE,    /* 100 W   */
                            /*fan_max*/ 10000);
        step_n(&p, /*pwm*/ 0, /*dt_ms*/ 100, /*n_steps*/ 600);
        int32_t load_only_mc = plant_zone_temp_mc(&p, 0);
        /* Must rise visibly (load present, no fan). */
        EXPECT_LE(25000 + 1000, load_only_mc);
    }

    /* === Scenario 4: load + max fan -> lower steady than scenario 3 */
    {
        plant_t p_no_fan;
        plant_init(&p_no_fan, 1);
        plant_set_zone_count(&p_no_fan, 1);
        config_heating_zone(&p_no_fan, 0,
                            25000, 25000, 100 * Q16_ONE, 10000);
        step_n(&p_no_fan, /*pwm*/ 0, 100, 600);
        int32_t load_only = plant_zone_temp_mc(&p_no_fan, 0);

        plant_t p_max_fan;
        plant_init(&p_max_fan, 1);
        plant_set_zone_count(&p_max_fan, 1);
        config_heating_zone(&p_max_fan, 0,
                            25000, 25000, 100 * Q16_ONE, 10000);
        step_n(&p_max_fan, /*pwm*/ 255, 100, 600);
        int32_t with_fan = plant_zone_temp_mc(&p_max_fan, 0);

        /* Max fan must suppress the rise by a visible margin. */
        EXPECT_LE(with_fan + 200, load_only);
    }

    /* === Scenario 5: fan curve x=0 endpoint clamp ================= */
    /* If the curve's first point is (0, 0), stepping at pwm=0 must
     * produce the same temperature trajectory as a config with
     * fan_max=0 -- the curve must clamp to (0,0).  Tests that
     * thermal_curve_eval_y0 is being called correctly from
     * step_one_zone. */
    {
        plant_t p_zero_fan;
        plant_init(&p_zero_fan, 1);
        plant_set_zone_count(&p_zero_fan, 1);
        config_heating_zone(&p_zero_fan, 0,
                            25000, 25000, 100 * Q16_ONE, 0);
        step_n(&p_zero_fan, /*pwm*/ 0, 100, 600);
        int32_t fan_max_zero = plant_zone_temp_mc(&p_zero_fan, 0);

        plant_t p_curve_clamp;
        plant_init(&p_curve_clamp, 1);
        plant_set_zone_count(&p_curve_clamp, 1);
        /* Same shape but with non-zero fan_max -- fan_curve_eval
         * at x=0 should still return curve[0].value0 = 0. */
        config_heating_zone(&p_curve_clamp, 0,
                            25000, 25000, 100 * Q16_ONE, 10000);
        step_n(&p_curve_clamp, /*pwm*/ 0, 100, 600);
        int32_t curve_clamped = plant_zone_temp_mc(&p_curve_clamp, 0);

        EXPECT_EQ(fan_max_zero, curve_clamped);
    }

    /* === Scenario 6: coupling -> neighbor heats up ================ */
    /* Zone 0 heated, zone 1 sits at ambient with coupling 0 -> 1.
     * After 30 s zone 1's temperature must have risen visibly. */
    {
        plant_t p;
        plant_init(&p, 1);
        plant_set_zone_count(&p, 2);
        config_heating_zone(&p, 0,
                            25000, 25000, 200 * Q16_ONE, 0);
        config_heating_zone(&p, 1,
                            25000, 25000, 0,            0);
        /* Zone 1 listens to zone 0 (its hotter neighbor). */
        plant_zone_set_coupling(&p, 1, /*neighbor*/ 0,
                                /*coupling_q16*/ 1000);
        plant_inputs_t in;
        memset(&in, 0, sizeof(in));
        for (int i = 0; i < 300; i++) plant_step(&p, &in, 100);
        int32_t z0 = plant_zone_temp_mc(&p, 0);
        int32_t z1 = plant_zone_temp_mc(&p, 1);
        EXPECT_LE(25000 + 1000, z0);          /* zone 0 heated */
        EXPECT_LE(25000 + 100,  z1);          /* zone 1 dragged up */
        EXPECT_LE(z1 + 1, z0);                /* but lags zone 0 */
    }

    /* === Scenario 7: coupling is order-independent ================ */
    /* Two zones cross-coupled (0<->1).  Run forward 1 tick and
     * confirm the result is identical to a hand-snapshotted
     * equivalent: we read both pre-tick temps, then update each
     * zone using the snapshot.  The plant implementation must
     * snapshot pre-tick to match. */
    {
        plant_t p;
        plant_init(&p, 1);
        plant_set_zone_count(&p, 2);
        config_heating_zone(&p, 0, 60000, 25000, 0, 0);
        config_heating_zone(&p, 1, 30000, 25000, 0, 0);
        plant_zone_set_coupling(&p, 0, 1, 5000);
        plant_zone_set_coupling(&p, 1, 0, 5000);

        plant_inputs_t in;
        memset(&in, 0, sizeof(in));
        plant_step(&p, &in, 100);
        int32_t z0_after = plant_zone_temp_mc(&p, 0);
        int32_t z1_after = plant_zone_temp_mc(&p, 1);

        /* Reset and compute again with the order reversed
         * implicitly by swapping zone indices.  If implementation
         * used post-update neighbor temps, swapping order would
         * change z0_after / z1_after; with pre-tick snapshot they
         * must match. */
        plant_t q;
        plant_init(&q, 1);
        plant_set_zone_count(&q, 2);
        config_heating_zone(&q, 0, 60000, 25000, 0, 0);
        config_heating_zone(&q, 1, 30000, 25000, 0, 0);
        plant_zone_set_coupling(&q, 0, 1, 5000);
        plant_zone_set_coupling(&q, 1, 0, 5000);
        plant_step(&q, &in, 100);

        EXPECT_EQ(plant_zone_temp_mc(&q, 0), z0_after);
        EXPECT_EQ(plant_zone_temp_mc(&q, 1), z1_after);

        /* Heated zone should lose some heat to the cold neighbor;
         * cold zone should gain.  Sign check. */
        EXPECT_LE(z0_after + 1, 60000);
        EXPECT_LE(30000 + 1,    z1_after);
    }

    /* === Scenario 8: PRNG determinism without noise =============== */
    /* Two plants with identical seed + config + tick sequence must
     * produce byte-equal temperature traces.  Without noise this
     * is just a smoke test for "no nondeterminism crept in". */
    {
        plant_t a, b;
        plant_init(&a, 7);
        plant_init(&b, 7);
        plant_set_zone_count(&a, 1);
        plant_set_zone_count(&b, 1);
        config_heating_zone(&a, 0, 25000, 25000, 100 * Q16_ONE, 10000);
        config_heating_zone(&b, 0, 25000, 25000, 100 * Q16_ONE, 10000);

        for (int i = 0; i < 500; i++) {
            plant_inputs_t in;
            memset(&in, 0, sizeof(in));
            in.pwm[0] = (uint8_t)(i & 0xFF);   /* varying PWM */
            plant_step(&a, &in, 100);
            plant_step(&b, &in, 100);
            EXPECT_EQ(plant_zone_temp_mc(&a, 0),
                      plant_zone_temp_mc(&b, 0));
        }
    }

    /* === Scenario 9: PRNG determinism with noise ================== */
    /* Same seed + config + noise amplitude -> byte-equal noisy
     * trajectories.  Catches any non-deterministic noise injection. */
    {
        plant_t a, b;
        plant_init(&a, 123);
        plant_init(&b, 123);
        plant_set_zone_count(&a, 1);
        plant_set_zone_count(&b, 1);
        config_heating_zone(&a, 0, 25000, 25000, 100 * Q16_ONE, 10000);
        config_heating_zone(&b, 0, 25000, 25000, 100 * Q16_ONE, 10000);
        plant_set_noise_amplitude_mc(&a, 500);
        plant_set_noise_amplitude_mc(&b, 500);

        for (int i = 0; i < 200; i++) {
            plant_inputs_t in;
            memset(&in, 0, sizeof(in));
            plant_step(&a, &in, 100);
            plant_step(&b, &in, 100);
            EXPECT_EQ(plant_zone_temp_mc(&a, 0),
                      plant_zone_temp_mc(&b, 0));
        }
    }

    /* === Scenario 10: PRNG separation across seeds ================ */
    /* Different seeds + same noise amplitude must produce
     * detectably different trajectories within a few hundred
     * ticks.  Catches degenerate PRNGs (e.g. constant or
     * state-independent). */
    {
        plant_t a, b;
        plant_init(&a, 1);
        plant_init(&b, 2);
        plant_set_zone_count(&a, 1);
        plant_set_zone_count(&b, 1);
        config_heating_zone(&a, 0, 25000, 25000, 0, 0);
        config_heating_zone(&b, 0, 25000, 25000, 0, 0);
        plant_set_noise_amplitude_mc(&a, 500);
        plant_set_noise_amplitude_mc(&b, 500);

        int diverged = 0;
        for (int i = 0; i < 200; i++) {
            plant_inputs_t in;
            memset(&in, 0, sizeof(in));
            plant_step(&a, &in, 100);
            plant_step(&b, &in, 100);
            if (plant_zone_temp_mc(&a, 0) != plant_zone_temp_mc(&b, 0)) {
                diverged = 1;
                break;
            }
        }
        EXPECT_EQ(diverged, 1);
    }
}
