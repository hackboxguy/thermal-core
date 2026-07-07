/* test/unit/test_config_hash.c
 *
 * Tests for support/thermal_config_hash.{c,h} and support/sha256.c.
 *
 * Scenarios:
 *   1. SHA-256 self-test against FIPS 180-4 vectors ("" and "abc").
 *   2. Hash determinism: identical configs hash equal.
 *   3. Padding poison: two configs memset'd to 0x00 vs 0xAA but
 *      identically initialised field-by-field hash equal.
 *   4. Field sensitivity: changing one int field flips the digest.
 *   5. Count sensitivity: bumping sensor_count without populating the
 *      added slot still produces a different digest (count is part of
 *      the canonical encoding).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "harness.h"
#include "thermal_core.h"
#include "thermal_config.h"
#include "sha256.h"
#include "thermal_config_hash.h"

/* === Helpers ======================================================= */

/* Hex-decode 64 chars into 32 bytes. */
static void hex32(const char *hex, uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        unsigned int b;
        sscanf(hex + i * 2, "%2x", &b);
        out[i] = (uint8_t)b;
    }
}

static int bytes_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

static void hex_print(const uint8_t *buf, size_t n, char *out, size_t out_sz)
{
    size_t off = 0;
    for (size_t i = 0; i < n && off + 3 < out_sz; i++) {
        off += (size_t)snprintf(out + off, out_sz - off, "%02x", buf[i]);
    }
}

/* Fill `cfg` with a small valid-ish layout we can hash.  Caller is
 * responsible for memset'ing the struct first; this function does
 * NOT memset, so padding state from before the call leaks through
 * (deliberate: that's what the padding-poison scenario exercises). */
static void init_minimal_valid(thermal_config_t *cfg)
{
    cfg->config_version    = 1;
    cfg->control_period_ms = 100;
    cfg->period_relative_to_ms = 0;

    cfg->sensor_count = 1;
    cfg->sensors[0].id = 0;
    /* Need NUL-terminated name within THERMAL_NAME_MAX; memcpy then
     * NUL-terminate so we don't depend on the pre-state of the
     * tail bytes (those are part of the struct's padding region). */
    memset(cfg->sensors[0].name, 0, sizeof(cfg->sensors[0].name));
    memcpy(cfg->sensors[0].name, "soc", 3);
    cfg->sensors[0].iir_alpha_q16    = 16384;
    cfg->sensors[0].max_staleness_ms = 500;

    cfg->actuator_count = 1;
    cfg->actuators[0].id = 0;
    memset(cfg->actuators[0].name, 0, sizeof(cfg->actuators[0].name));
    memcpy(cfg->actuators[0].name, "fan", 3);
    cfg->actuators[0].pwm_min       = 80;
    cfg->actuators[0].pwm_max       = 255;
    cfg->actuators[0].slew_per_tick = 0;
    cfg->actuators[0].spinup_pwm    = 0;
    cfg->actuators[0].spinup_ms     = 0;
    cfg->actuators[0].min_on_ticks  = 0;
    cfg->actuators[0].min_off_ticks = 0;
    cfg->actuators[0].state_pwm[0]  = 0;
    cfg->actuators[0].state_pwm[1]  = 100;
    cfg->actuators[0].state_pwm[2]  = 160;
    cfg->actuators[0].state_pwm[3]  = 220;
    cfg->actuators[0].state_pwm[4]  = 255;
#if THERMALCORE_ENABLE_PID
    memset(cfg->actuators[0].duty_linearization, 0,
           sizeof(cfg->actuators[0].duty_linearization));
    cfg->actuators[0].duty_linearization_count = 0;
#endif

    cfg->context_count  = 0;
    cfg->zone_count     = 0;
    cfg->modifier_count = 0;
    memset(&cfg->faults,   0, sizeof(cfg->faults));
    memset(&cfg->telemetry, 0, sizeof(cfg->telemetry));
#if THERMALCORE_ENABLE_FAN_HEALTH
    /* Compound region the field-by-field init above does not cover;
     * the canonical hash encodes fan_health for every active actuator
     * slot, so zero it like faults/telemetry. */
    memset(&cfg->fan_health, 0, sizeof(cfg->fan_health));
#endif
}

TEST_CASE(config_hash) {
    /* === Scenario 1: SHA-256 self-test ============================ */
    {
        sha256_ctx_t ctx;
        uint8_t      digest[32];
        uint8_t      expect[32];

        /* FIPS 180-4 / RFC 6234 — empty string */
        sha256_init(&ctx);
        sha256_final(&ctx, digest);
        hex32("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              expect);
        if (!bytes_equal(digest, expect, 32)) {
            char a[80], b[80];
            hex_print(digest, 32, a, sizeof(a));
            hex_print(expect, 32, b, sizeof(b));
            fprintf(stderr, "sha256(\"\") got %s, expected %s\n", a, b);
            exit(1);
        }

        /* "abc" — classic test vector */
        sha256_init(&ctx);
        sha256_update(&ctx, "abc", 3);
        sha256_final(&ctx, digest);
        hex32("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              expect);
        if (!bytes_equal(digest, expect, 32)) {
            char a[80], b[80];
            hex_print(digest, 32, a, sizeof(a));
            hex_print(expect, 32, b, sizeof(b));
            fprintf(stderr, "sha256(\"abc\") got %s, expected %s\n", a, b);
            exit(1);
        }
    }

    /* === Scenario 2: identical configs -> identical hash ========== */
    {
        thermal_config_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        init_minimal_valid(&a);
        init_minimal_valid(&b);
        uint8_t ha[32], hb[32];
        thermal_config_hash(&a, ha);
        thermal_config_hash(&b, hb);
        if (!bytes_equal(ha, hb, 32)) {
            fprintf(stderr, "identical configs hashed differently\n");
            exit(1);
        }
    }

    /* === Scenario 3: padding poison ============================== */
    {
        thermal_config_t a, b;
        memset(&a, 0x00, sizeof(a));     /* clean padding */
        memset(&b, 0xAA, sizeof(b));     /* poisoned padding */
        init_minimal_valid(&a);
        init_minimal_valid(&b);
        uint8_t ha[32], hb[32];
        thermal_config_hash(&a, ha);
        thermal_config_hash(&b, hb);
        if (!bytes_equal(ha, hb, 32)) {
            char sa[80], sb[80];
            hex_print(ha, 32, sa, sizeof(sa));
            hex_print(hb, 32, sb, sizeof(sb));
            fprintf(stderr,
                    "padding poison detected divergence: %s vs %s\n",
                    sa, sb);
            exit(1);
        }
    }

    /* === Scenario 4: field sensitivity =========================== */
    {
        thermal_config_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        init_minimal_valid(&a);
        init_minimal_valid(&b);
        b.sensors[0].iir_alpha_q16 = 32768;   /* one bit flipped */
        uint8_t ha[32], hb[32];
        thermal_config_hash(&a, ha);
        thermal_config_hash(&b, hb);
        if (bytes_equal(ha, hb, 32)) {
            fprintf(stderr,
                    "field change did not affect digest (insensitivity)\n");
            exit(1);
        }
    }

    /* === Scenario 4b: fault threshold2 sensitivity ================ */
    {
        thermal_config_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        init_minimal_valid(&a);
        init_minimal_valid(&b);
        b.faults.stuck_sensor_defaults.threshold2 = 1;
        uint8_t ha[32], hb[32];
        thermal_config_hash(&a, ha);
        thermal_config_hash(&b, hb);
        if (bytes_equal(ha, hb, 32)) {
            fprintf(stderr,
                    "threshold2 change did not affect digest\n");
            exit(1);
        }
    }

#if THERMALCORE_ENABLE_PID
    /* === Scenario 4c: active PID D-filter sensitivity ============= */
    {
        thermal_config_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        init_minimal_valid(&a);
        init_minimal_valid(&b);
        a.zone_count = 1;
        b.zone_count = 1;
        b.zones[0].pid.d_filter_alpha_q16 = 32768;
        uint8_t ha[32], hb[32];
        thermal_config_hash(&a, ha);
        thermal_config_hash(&b, hb);
        if (bytes_equal(ha, hb, 32)) {
            fprintf(stderr,
                    "active d_filter_alpha_q16 change did not affect digest\n");
            exit(1);
        }
    }

    /* === Scenario 4d: inactive PID D-filter slot is zero-filled ==== */
    {
        thermal_config_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        init_minimal_valid(&a);
        init_minimal_valid(&b);
        b.zones[0].pid.d_filter_alpha_q16 = 32768;
        uint8_t ha[32], hb[32];
        thermal_config_hash(&a, ha);
        thermal_config_hash(&b, hb);
        if (!bytes_equal(ha, hb, 32)) {
            fprintf(stderr,
                    "inactive d_filter_alpha_q16 leaked into digest\n");
            exit(1);
        }
    }
#endif

#if THERMALCORE_ENABLE_PID
    /* === Scenario 4e: active duty linearization sensitivity ======== */
    {
        thermal_config_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        init_minimal_valid(&a);
        init_minimal_valid(&b);
        b.actuators[0].duty_linearization_count = 3;
        b.actuators[0].duty_linearization[0].x = 0;
        b.actuators[0].duty_linearization[0].value0 = 0;
        b.actuators[0].duty_linearization[1].x = 99;
        b.actuators[0].duty_linearization[1].value0 = 150;
        b.actuators[0].duty_linearization[2].x = 255;
        b.actuators[0].duty_linearization[2].value0 = 255;
        uint8_t ha[32], hb[32];
        thermal_config_hash(&a, ha);
        thermal_config_hash(&b, hb);
        if (bytes_equal(ha, hb, 32)) {
            fprintf(stderr,
                    "active duty_linearization change did not affect digest\n");
            exit(1);
        }
    }

    /* === Scenario 4f: inactive duty linearization is zero-filled === */
    {
        thermal_config_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        init_minimal_valid(&a);
        init_minimal_valid(&b);
        a.actuator_count = 0;
        b.actuator_count = 0;
        b.actuators[0].duty_linearization_count = 3;
        b.actuators[0].duty_linearization[0].x = 0;
        b.actuators[0].duty_linearization[0].value0 = 0;
        b.actuators[0].duty_linearization[1].x = 99;
        b.actuators[0].duty_linearization[1].value0 = 150;
        b.actuators[0].duty_linearization[2].x = 255;
        b.actuators[0].duty_linearization[2].value0 = 255;
        uint8_t ha[32], hb[32];
        thermal_config_hash(&a, ha);
        thermal_config_hash(&b, hb);
        if (!bytes_equal(ha, hb, 32)) {
            fprintf(stderr,
                    "inactive duty_linearization leaked into digest\n");
            exit(1);
        }
    }
#endif

#if THERMAL_MAX_SENSORS >= 2  /* bumps sensor_count to 2; skipped under a maxima=1 profile */
    /* === Scenario 5: count sensitivity =========================== */
    {
        thermal_config_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        init_minimal_valid(&a);
        init_minimal_valid(&b);
        /* Bump sensor_count to 2 without populating slot 1; that
         * empty slot is still encoded because count grew. */
        b.sensor_count = 2;
        uint8_t ha[32], hb[32];
        thermal_config_hash(&a, ha);
        thermal_config_hash(&b, hb);
        if (bytes_equal(ha, hb, 32)) {
            fprintf(stderr,
                    "sensor_count change did not affect digest\n");
            exit(1);
        }
    }
#endif  /* Scenario 5 */
}
