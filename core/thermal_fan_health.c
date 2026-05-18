/* core/thermal_fan_health.c -- fan-health detector, Stage 17, PRD App C.
 *
 * Advisory-only negative-RPM-drift detector. See thermal_fan_health.h
 * for the contract. The whole body is behind THERMALCORE_ENABLE_FAN_HEALTH
 * so the feature contributes zero bytes when compiled out.
 */
#include "thermal_fan_health.h"

#if THERMALCORE_ENABLE_FAN_HEALTH

#include "thermal_curve.h"

/* Round num/den to the nearest integer, halves away from zero. `den`
 * must be > 0. C99 `/` truncates toward zero, so adding/subtracting
 * den before dividing yields exact round-half-away-from-zero. */
static int32_t div_round_half_away(int32_t num, int32_t den) {
    if (num >= 0) {
        return (2 * num + den) / (2 * den);
    }
    return (2 * num - den) / (2 * den);
}

/* Baseline point whose PWM is closest to `pwm` (ties -> lower index). */
static uint8_t nearest_point(const thermal_fan_health_cfg_t *cfg,
                             uint8_t pwm) {
    uint8_t best = 0;
    int32_t best_d = -1;
    for (uint8_t i = 0; i < cfg->baseline_count; i++) {
        int32_t d = (int32_t)pwm - cfg->baseline[i].x;
        if (d < 0) d = -d;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    return best;
}

/* Re-derive the aggregate health_delta_pct, confidence, and severity
 * from the per-point accumulators. Called after a delta is folded. */
static void recompute(thermal_fan_health_state_t *s,
                      const thermal_fan_health_cfg_t *cfg) {
    int32_t sum_wx = 0;          /* sum of weight * ema_x256          */
    int32_t sum_w  = 0;          /* sum of weight                     */
    uint8_t conf   = 0;

    for (uint8_t p = 0; p < cfg->baseline_count; p++) {
        if (s->sample_count[p] == 0) {
            continue;
        }
        conf++;
        /* Weight = clamped observed-sample count, mid-emphasised:
         * interior baseline points x2 (best signal-to-noise),
         * endpoints x1 (noisy near stall, voltage-limited at 100%). */
        uint16_t sc = s->sample_count[p];
        if (sc > 16u) {
            sc = 16u;
        }
        int32_t emph = (p == 0 || p == (uint8_t)(cfg->baseline_count - 1))
                           ? 1 : 2;
        int32_t w = (int32_t)sc * emph;
        sum_w  += w;
        sum_wx += w * s->ema_x256[p];
    }

    s->confidence = conf;
    s->health_delta_pct =
        (sum_w > 0)
            ? (int16_t)div_round_half_away(sum_wx, sum_w * 256)
            : (int16_t)0;

    /* Severity: HEALTHY until enough baseline points carry evidence;
     * a model-generic baseline suppresses AGING (PRD C.3). */
    if (s->confidence < cfg->min_points_observed) {
        s->severity = THERMAL_FAN_HEALTH_HEALTHY;
        return;
    }
    int32_t d = s->health_delta_pct;
    if (d <= cfg->failing_pct) {
        s->severity = THERMAL_FAN_HEALTH_FAILING;
    } else if (d <= cfg->degraded_pct) {
        s->severity = THERMAL_FAN_HEALTH_DEGRADED;
    } else if (cfg->baseline_source != THERMAL_FAN_BASELINE_SRC_MODEL &&
               d <= cfg->aging_pct) {
        s->severity = THERMAL_FAN_HEALTH_AGING;
    } else {
        s->severity = THERMAL_FAN_HEALTH_HEALTHY;
    }
}

void thermal_fan_health_reset(thermal_fan_health_state_t *s) {
    s->have_ref         = 0;
    s->ref_pwm          = 0;
    s->pwm_run          = 0;
    s->prev_rpm         = 0;
    s->rpm_run          = 0;
    s->prev_pwm_low     = 1;     /* first rise past pwm_min arms spin-up */
    s->spinup_remaining = 0;
    s->prev_override    = 0;
    for (uint8_t i = 0; i < THERMAL_MAX_FAN_HEALTH_POINTS; i++) {
        s->ema_x256[i]     = 0;
        s->sample_count[i] = 0;
    }
    s->health_delta_pct = 0;
    s->severity         = THERMAL_FAN_HEALTH_HEALTHY;
    s->confidence       = 0;
}

void thermal_fan_health_step(thermal_fan_health_state_t *s,
                             const thermal_fan_health_cfg_t *cfg,
                             uint8_t  applied_pwm,
                             uint16_t tach_rpm,
                             uint8_t  tach_valid,
                             uint8_t  slew_limited,
                             uint8_t  fault_override,
                             uint16_t spinup_ticks) {
    if (!cfg->enable || cfg->baseline_count < 2) {
        return;
    }

    uint8_t pwm_min = (uint8_t)cfg->baseline[0].x;

    /* Spin-up grace (self-tracked): a rise from below the baseline's
     * lowest PWM up into range arms a countdown of skipped ticks. */
    uint8_t pwm_low = (applied_pwm < pwm_min);
    if (s->prev_pwm_low && !pwm_low) {
        s->spinup_remaining = spinup_ticks;
    }
    s->prev_pwm_low = pwm_low;
    uint8_t in_spinup = (s->spinup_remaining > 0);
    if (in_spinup) {
        s->spinup_remaining--;
    }

    /* Fault-override window: the tach sample lags the drive by one
     * control period, so an override skips both its own tick and the
     * next one. */
    uint8_t override_window = (fault_override || s->prev_override);
    s->prev_override = fault_override;

    /* Gate: a tick that is not free-running steady operation is no
     * evidence about aging -- drop it and restart the steady window. */
    if (!tach_valid || pwm_low || in_spinup || slew_limited ||
        override_window) {
        s->have_ref = 0;
        s->pwm_run  = 0;
        s->rpm_run  = 0;
        return;
    }

    /* Steady-window tracking. */
    if (!s->have_ref) {
        s->have_ref = 1;
        s->ref_pwm  = applied_pwm;
        s->pwm_run  = 1;
        s->prev_rpm = tach_rpm;
        s->rpm_run  = 1;
        return;
    }

    int pwm_dev = (int)applied_pwm - (int)s->ref_pwm;
    if (pwm_dev < 0) {
        pwm_dev = -pwm_dev;
    }
    if (pwm_dev > (int)cfg->stable_pwm_tolerance) {
        /* Operating point moved -- open a fresh window here. */
        s->ref_pwm  = applied_pwm;
        s->pwm_run  = 1;
        s->prev_rpm = tach_rpm;
        s->rpm_run  = 1;
        return;
    }
    if (s->pwm_run < 0xFFFFu) {
        s->pwm_run++;
    }

    int32_t rpm_change = (int32_t)tach_rpm - (int32_t)s->prev_rpm;
    if (rpm_change < 0) {
        rpm_change = -rpm_change;
    }
    int32_t rpm_tol = (int32_t)cfg->stable_rpm_tolerance_pct *
                      (int32_t)s->prev_rpm;
    if (rpm_change * 100 <= rpm_tol) {
        if (s->rpm_run < 0xFFFFu) {
            s->rpm_run++;
        }
    } else {
        s->rpm_run = 1;
    }
    s->prev_rpm = tach_rpm;

    if (s->pwm_run < cfg->stable_pwm_ticks ||
        s->rpm_run < cfg->stable_rpm_ticks) {
        return;                  /* not steady long enough yet */
    }

    /* Steady: fold one delta into the nearest baseline point. The
     * expected RPM is interpolated between adjacent baseline points
     * (thermal_curve.c discipline), so an arbitrary steady governor
     * duty still yields a delta. */
    int32_t expected = thermal_curve_eval_y0(cfg->baseline,
                                             cfg->baseline_count,
                                             (int32_t)applied_pwm);
    if (expected <= 0) {
        return;                  /* defensive -- validation forbids */
    }

    int32_t delta = div_round_half_away(
        ((int32_t)tach_rpm - expected) * 100, expected);
    if (delta >  127) delta =  127;
    if (delta < -127) delta = -127;

    uint8_t np = nearest_point(cfg, applied_pwm);
    int32_t target = delta * 256;
    if (s->sample_count[np] == 0) {
        s->ema_x256[np] = target;
    } else {
        s->ema_x256[np] += (target - s->ema_x256[np]) / 8;
    }
    if (s->sample_count[np] < 0xFFFFu) {
        s->sample_count[np]++;
    }

    recompute(s, cfg);
}

#else
/* Feature compiled out: a single declaration keeps this a valid,
 * non-empty translation unit under -pedantic. */
typedef int thermal_fan_health_disabled_tu_t;
#endif /* THERMALCORE_ENABLE_FAN_HEALTH */
