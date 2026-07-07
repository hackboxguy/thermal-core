/* support/thermal_config_hash.c
 *
 * Canonical encoder + SHA-256 wrapper.  See header for contract.
 *
 * Encoding order (locked; future changes are deliberate hash-
 * breaking PRs that bump config_version, not silent edits):
 *
 *   1. config_version            (u16 LE)
 *   2. control_period_ms         (u16 LE)
 *      period_relative_to_ms     (u16 LE; 0 if unspecified)
 *   3. sensors[0..MAX-1]         field-by-field, zero-filled past
 *                                 sensor_count
 *      sensor_count              (u8)
 *   4. contexts[0..MAX-1]        same shape
 *      context_count             (u8)
 *   5. actuators[0..MAX-1]       same shape
 *      actuator_count            (u8)
 *   6. zones[0..MAX-1]           field-by-field (incl. nested
 *                                 sensor_ids[], sensor_weights[],
 *                                 actuator_ids[], trips[], pid)
 *      zone_count                (u8)
 *   7. modifiers[0..MAX-1]       field-by-field (incl. curve[])
 *      modifier_count            (u8)
 *   8. faults: stall, stuck_sensor, runaway, stale_context defaults
 *      (each a thermal_fault_detector_cfg_t)
 *   9. telemetry: enable (u8), period_ticks (u16 LE),
 *      enabled_signal_ids[0..MAX-1] (each u16 LE),
 *      enabled_signal_count (u8)
 *  10. fan_health[0..MAX_ACTUATORS-1] field-by-field, zero-filled
 *      past actuator_count -- only when THERMALCORE_ENABLE_FAN_HEALTH
 *      is defined (Stage 17, PRD Appendix C). Compiled out, this
 *      stage is absent and the digest is unchanged from v1.
 *
 * The digest is intentionally feature-profile scoped: compile-time
 * gates such as THERMALCORE_ENABLE_PID and THERMALCORE_ENABLE_FAN_HEALTH
 * add or omit their owned fields from this canonical stream.
 */
#include "thermal_config_hash.h"

#include <string.h>

/* === Primitive writers (feed SHA-256 directly) ===================== */

static void w_u8(sha256_ctx_t *s, uint8_t v)
{
    sha256_update(s, &v, 1);
}

static void w_u16(sha256_ctx_t *s, uint16_t v)
{
    const uint8_t b[2] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu) };
    sha256_update(s, b, 2);
}

static void w_u32(sha256_ctx_t *s, uint32_t v)
{
    const uint8_t b[4] = {
        (uint8_t)( v        & 0xFFu),
        (uint8_t)((v >>  8) & 0xFFu),
        (uint8_t)((v >> 16) & 0xFFu),
        (uint8_t)((v >> 24) & 0xFFu),
    };
    sha256_update(s, b, 4);
}

static void w_i32(sha256_ctx_t *s, int32_t v)
{
    w_u32(s, (uint32_t)v);
}

/* Names: exactly THERMAL_NAME_MAX bytes, NUL-terminated and zero-
 * padded.  We re-derive the byte form here rather than memcpy'ing
 * the struct field so padding/garbage past the NUL never reaches
 * the hash even when the caller skipped memset(&cfg, 0, ...).
 */
static void w_name(sha256_ctx_t *s, const char *name)
{
    uint8_t buf[THERMAL_NAME_MAX];
    memset(buf, 0, sizeof(buf));
    if (name) {
        /* strnlen is POSIX-only; do it by hand to stay portable C99. */
        size_t n = 0;
        while (n < THERMAL_NAME_MAX && name[n] != '\0') n++;
        memcpy(buf, name, n);
    }
    sha256_update(s, buf, sizeof(buf));
}

/* Walk a fixed-size uint8 array to the compile-time maximum, zero-
 * filling past `count`.  Used for state_pwm. */
static void w_u8_array(sha256_ctx_t *s, const uint8_t *arr,
                       uint8_t count, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        w_u8(s, i < count ? arr[i] : 0u);
    }
}

/* Walk a fixed-size uint16 array (no count gate — caller passes the
 * effective length via `count`).  Used for sensor_ids, actuator_ids,
 * enabled_signal_ids. */
static void w_u16_array(sha256_ctx_t *s, const uint16_t *arr,
                        uint8_t count, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        w_u16(s, i < count ? arr[i] : 0u);
    }
}

/* Walk a fixed-size int32 array. */
static void w_i32_array(sha256_ctx_t *s, const int32_t *arr,
                        uint8_t count, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        w_i32(s, i < count ? arr[i] : 0);
    }
}

/* === Compound writers ============================================== */

static void w_curve_point(sha256_ctx_t *s, const thermal_curve_point_t *c, int active);

static void w_sensor(sha256_ctx_t *s, const thermal_sensor_cfg_t *c, int active)
{
    if (!active) { uint8_t z[THERMAL_NAME_MAX + 2 + 4 + 4] = {0};
                   sha256_update(s, z, sizeof(z)); return; }
    w_u16 (s, c->id);
    w_name(s, c->name);
    w_i32 (s, c->iir_alpha_q16);
    w_u32 (s, c->max_staleness_ms);
}

static void w_context(sha256_ctx_t *s, const thermal_context_cfg_t *c, int active)
{
    if (!active) { uint8_t z[THERMAL_NAME_MAX + 2 + 1 + 4 + 4 + 1] = {0};
                   sha256_update(s, z, sizeof(z)); return; }
    w_u16 (s, c->id);
    w_name(s, c->name);
    w_u8  (s, c->unit);
    w_i32 (s, c->iir_alpha_q16);
    w_u32 (s, c->timeout_ms);
    w_u8  (s, c->fail_safe);
}

static void w_actuator(sha256_ctx_t *s, const thermal_actuator_cfg_t *c, int active)
{
    if (!active) {
        uint8_t z[THERMAL_NAME_MAX + 2 + 1 + 1 + 1 + 1 + 4 + 2 + 2 +
                  THERMAL_MAX_COOLING_STATES] = {0};
        sha256_update(s, z, sizeof(z));
#if THERMALCORE_ENABLE_PID
        for (size_t i = 0; i < THERMAL_MAX_CURVE_POINTS; i++) {
            w_curve_point(s, NULL, 0);
        }
        w_u8(s, 0);
#endif
        return;
    }
    w_u16(s, c->id);
    w_name(s, c->name);
    w_u8 (s, c->pwm_min);
    w_u8 (s, c->pwm_max);
    w_u8 (s, c->slew_per_tick);
    w_u8 (s, c->spinup_pwm);
    w_u32(s, c->spinup_ms);
    w_u16(s, c->min_on_ticks);
    w_u16(s, c->min_off_ticks);
    w_u8_array(s, c->state_pwm,
               (uint8_t)THERMAL_MAX_COOLING_STATES,
               (size_t)THERMAL_MAX_COOLING_STATES);
#if THERMALCORE_ENABLE_PID
    for (size_t i = 0; i < THERMAL_MAX_CURVE_POINTS; i++) {
        w_curve_point(s, &c->duty_linearization[i],
                      i < c->duty_linearization_count);
    }
    w_u8(s, c->duty_linearization_count);
#endif
}

static void w_pid(sha256_ctx_t *s, const thermal_pid_cfg_t *p)
{
    w_i32(s, p->kp_q16);
    w_i32(s, p->ki_q16);
    w_i32(s, p->kd_q16);
#if THERMALCORE_ENABLE_PID
    w_i32(s, p->d_filter_alpha_q16);
#endif
    w_i32(s, p->setpoint_mc);
    w_i32(s, p->kp_min_q16);  w_i32(s, p->kp_max_q16);
    w_i32(s, p->ki_min_q16);  w_i32(s, p->ki_max_q16);
    w_i32(s, p->kd_min_q16);  w_i32(s, p->kd_max_q16);
    w_i32(s, p->setpoint_min_mc);
    w_i32(s, p->setpoint_max_mc);
    w_u16(s, p->dt_min_ms);
    w_u16(s, p->dt_max_ms);
}

static void w_trip(sha256_ctx_t *s, const thermal_trip_cfg_t *t, int active)
{
    if (!active) { uint8_t z[4 + 4 + 1 + 1] = {0};
                   sha256_update(s, z, sizeof(z)); return; }
    w_i32(s, t->temp_mc);
    w_i32(s, t->hyst_mc);
    w_u8 (s, t->severity);
    w_u8 (s, t->cooling_state);
}

static void w_zone(sha256_ctx_t *s, const thermal_zone_cfg_t *z, int active)
{
    if (!active) {
        /* name + sensor_ids + sensor_weights + count + aggregation +
         * fallback + governor + pid + actuator_ids + count +
         * trips(4 * 10) + count */
        w_name(s, NULL);
        w_u16_array(s, NULL, 0, THERMAL_MAX_SENSORS_PER_ZONE);
        w_i32_array(s, NULL, 0, THERMAL_MAX_SENSORS_PER_ZONE);
        w_u8 (s, 0);
        w_u8 (s, 0);
        w_i32(s, 0);
        w_u8 (s, 0);
        thermal_pid_cfg_t zpid; memset(&zpid, 0, sizeof(zpid));
        w_pid(s, &zpid);
        w_u16_array(s, NULL, 0, THERMAL_MAX_ACTUATORS_PER_ZONE);
        w_u8 (s, 0);
        for (size_t i = 0; i < THERMAL_MAX_TRIPS_PER_ZONE; i++) w_trip(s, NULL, 0);
        w_u8 (s, 0);
        return;
    }
    w_name(s, z->name);
    w_u16_array(s, z->sensor_ids, z->sensor_count, THERMAL_MAX_SENSORS_PER_ZONE);
    w_i32_array(s, z->sensor_weights_q16, z->sensor_count, THERMAL_MAX_SENSORS_PER_ZONE);
    w_u8(s, z->sensor_count);
    w_u8(s, z->aggregation);
    w_i32(s, z->fallback_temp_mc);
    w_u8(s, z->governor);
    w_pid(s, &z->pid);
    w_u16_array(s, z->actuator_ids, z->actuator_count, THERMAL_MAX_ACTUATORS_PER_ZONE);
    w_u8(s, z->actuator_count);
    for (size_t i = 0; i < THERMAL_MAX_TRIPS_PER_ZONE; i++) {
        w_trip(s, &z->trips[i], i < z->trip_count);
    }
    w_u8(s, z->trip_count);
}

static void w_curve_point(sha256_ctx_t *s, const thermal_curve_point_t *c, int active)
{
    if (!active) { uint8_t z[4 + 4 + 4] = {0};
                   sha256_update(s, z, sizeof(z)); return; }
    w_i32(s, c->x);
    w_i32(s, c->value0);
    w_i32(s, c->value1);
}

static void w_modifier(sha256_ctx_t *s, const thermal_modifier_cfg_t *m, int active)
{
    if (!active) {
        w_name(s, NULL);
        w_u16(s, 0);
        w_u8 (s, 0);
        for (size_t i = 0; i < THERMAL_MAX_CURVE_POINTS; i++) w_curve_point(s, NULL, 0);
        w_u8 (s, 0);
        w_u8 (s, 0);
        return;
    }
    w_name(s, m->name);
    w_u16 (s, m->context_id);
    w_u8  (s, m->stages);
    for (size_t i = 0; i < THERMAL_MAX_CURVE_POINTS; i++) {
        w_curve_point(s, &m->curve[i], i < m->curve_count);
    }
    w_u8(s, m->curve_count);
    w_u8(s, m->fail_safe);
}

static void w_fault_detector(sha256_ctx_t *s, const thermal_fault_detector_cfg_t *f)
{
    w_u8 (s, f->enabled);
    w_u8 (s, f->severity);
    w_u8 (s, f->action);
    w_u16(s, f->persist_ticks);
    w_u16(s, f->recovery_ticks);
    w_i32(s, f->threshold0);
    w_i32(s, f->threshold1);
    w_i32(s, f->threshold2);
    w_u16(s, f->correlated_context_id);
}

#if THERMALCORE_ENABLE_FAN_HEALTH
static void w_fan_health(sha256_ctx_t *s, const thermal_fan_health_cfg_t *f,
                         int active)
{
    if (!active) {
        w_u8(s, 0); w_u8(s, 0);
        for (size_t i = 0; i < THERMAL_MAX_FAN_HEALTH_POINTS; i++) {
            w_curve_point(s, NULL, 0);
        }
        w_u8(s, 0);
        w_u16(s, 0); w_u8(s, 0); w_u16(s, 0); w_u8(s, 0);
        w_u8(s, 0); w_u8(s, 0); w_u8(s, 0); w_u8(s, 0);
        return;
    }
    w_u8(s, f->enable);
    w_u8(s, f->baseline_source);
    for (size_t i = 0; i < THERMAL_MAX_FAN_HEALTH_POINTS; i++) {
        w_curve_point(s, &f->baseline[i], i < f->baseline_count);
    }
    w_u8 (s, f->baseline_count);
    w_u16(s, f->stable_pwm_ticks);
    w_u8 (s, f->stable_pwm_tolerance);
    w_u16(s, f->stable_rpm_ticks);
    w_u8 (s, f->stable_rpm_tolerance_pct);
    w_u8 (s, f->min_points_observed);
    w_u8 (s, (uint8_t)f->aging_pct);
    w_u8 (s, (uint8_t)f->degraded_pct);
    w_u8 (s, (uint8_t)f->failing_pct);
}
#endif /* THERMALCORE_ENABLE_FAN_HEALTH */

/* === Public entry point ============================================ */

void thermal_config_hash(const thermal_config_t *cfg,
                         uint8_t out[SHA256_DIGEST_LEN])
{
    sha256_ctx_t s;
    sha256_init(&s);

    w_u16(&s, cfg->config_version);
    w_u16(&s, cfg->control_period_ms);
    w_u16(&s, cfg->period_relative_to_ms);

    for (size_t i = 0; i < THERMAL_MAX_SENSORS; i++) {
        w_sensor(&s, &cfg->sensors[i], i < cfg->sensor_count);
    }
    w_u8(&s, cfg->sensor_count);

    for (size_t i = 0; i < THERMAL_MAX_CONTEXT_SIGNALS; i++) {
        w_context(&s, &cfg->contexts[i], i < cfg->context_count);
    }
    w_u8(&s, cfg->context_count);

    for (size_t i = 0; i < THERMAL_MAX_ACTUATORS; i++) {
        w_actuator(&s, &cfg->actuators[i], i < cfg->actuator_count);
    }
    w_u8(&s, cfg->actuator_count);

    for (size_t i = 0; i < THERMAL_MAX_ZONES; i++) {
        w_zone(&s, &cfg->zones[i], i < cfg->zone_count);
    }
    w_u8(&s, cfg->zone_count);

    for (size_t i = 0; i < THERMAL_MAX_MODIFIERS; i++) {
        w_modifier(&s, &cfg->modifiers[i], i < cfg->modifier_count);
    }
    w_u8(&s, cfg->modifier_count);

    w_fault_detector(&s, &cfg->faults.stall_defaults);
    w_fault_detector(&s, &cfg->faults.stuck_sensor_defaults);
    w_fault_detector(&s, &cfg->faults.runaway_defaults);
    w_fault_detector(&s, &cfg->faults.stale_context_defaults);

    w_u8 (&s, cfg->telemetry.enable);
    w_u16(&s, cfg->telemetry.period_ticks);
    w_u16_array(&s, cfg->telemetry.enabled_signal_ids,
                cfg->telemetry.enabled_signal_count,
                THERMAL_MAX_TELEMETRY_SIGNALS);
    w_u8(&s, cfg->telemetry.enabled_signal_count);

#if THERMALCORE_ENABLE_FAN_HEALTH
    for (size_t i = 0; i < THERMAL_MAX_ACTUATORS; i++) {
        w_fan_health(&s, &cfg->fan_health[i], i < cfg->actuator_count);
    }
#endif

    sha256_final(&s, out);
}
