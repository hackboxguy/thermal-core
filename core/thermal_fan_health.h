/* core/thermal_fan_health.h
 *
 * Fan-health detector -- Stage 17, PRD Appendix C. A compile-optional,
 * advisory-only module that compares measured tach RPM against a
 * known-good PWM-to-RPM baseline and grades negative RPM drift
 * (bearing wear, motor degradation, load-adding contamination). It is
 * the early-warning companion to the v1 stall detector: stall raises a
 * binary fault at the terminal failure, fan-health grades the approach.
 *
 * It NEVER commands an actuator. It produces only telemetry. It is not
 * a thermal_fault detector -- no FSM, no action, no fault state.
 *
 * Compile gate: the whole module is behind THERMALCORE_ENABLE_FAN_HEALTH.
 * Undefined (the MCU default) -> the module is empty and contributes
 * zero bytes. Host builds define it; ESP32/CH32 builds do not.
 *
 * Input alignment (PRD C.2): fan-health compares the tach sample
 * against the *applied* duty that produced it -- the previous control
 * period's final duty_0_255, since tach is read before the new command
 * is applied -- NOT the stall detector's pre-slew/pre-fault requested
 * PWM. The caller passes that previous applied duty as `applied_pwm`.
 */
#ifndef THERMAL_FAN_HEALTH_H
#define THERMAL_FAN_HEALTH_H

#include "thermal_config.h"

#if THERMALCORE_ENABLE_FAN_HEALTH

#include <stdint.h>
#include "thermal_types.h"     /* thermal_curve_point_t */

/* Severity ladder -- advisory grades, never a fault state. */
#define THERMAL_FAN_HEALTH_HEALTHY    0u
#define THERMAL_FAN_HEALTH_AGING      1u
#define THERMAL_FAN_HEALTH_DEGRADED   2u
#define THERMAL_FAN_HEALTH_FAILING    3u

/* Baseline provenance -- gates severity resolution (PRD C.3): a
 * model-generic baseline conflates aging with unit-to-unit variance,
 * so it asserts only DEGRADED/FAILING and suppresses AGING. NONE is
 * not config-authorable -- it is the telemetry value reported for an
 * actuator whose fan-health detector is disabled (PRD C.5), so a
 * consumer never mistakes a zeroed slot for a real `field` baseline. */
#define THERMAL_FAN_BASELINE_SRC_FIELD    0u
#define THERMAL_FAN_BASELINE_SRC_FACTORY  1u
#define THERMAL_FAN_BASELINE_SRC_MODEL    2u
#define THERMAL_FAN_BASELINE_SRC_NONE     3u

/* Per-actuator configuration -- const, supplied by the platform.
 * The baseline reuses thermal_curve_point_t: x = PWM (0..255),
 * value0 = expected RPM at that PWM. value1 is unused. */
typedef struct {
    uint8_t  enable;
    uint8_t  baseline_source;       /* THERMAL_FAN_BASELINE_SRC_* */
    thermal_curve_point_t baseline[THERMAL_MAX_FAN_HEALTH_POINTS];
    uint8_t  baseline_count;
    uint16_t stable_pwm_ticks;      /* PWM steady this long before sampling */
    uint8_t  stable_pwm_tolerance;  /* PWM counts of allowed deviation */
    uint16_t stable_rpm_ticks;      /* RPM settled this long (shorter window) */
    uint8_t  stable_rpm_tolerance_pct;
    uint8_t  min_points_observed;   /* confidence floor before severity > HEALTHY */
    int8_t   aging_pct;             /* signed: 0 > aging > degraded > failing */
    int8_t   degraded_pct;
    int8_t   failing_pct;
} thermal_fan_health_cfg_t;

/* Per-actuator runtime state -- fixed-size, no heap. */
typedef struct {
    /* steady-window tracking */
    uint8_t  have_ref;
    uint8_t  ref_pwm;
    uint16_t pwm_run;               /* consecutive ticks PWM held steady */
    uint16_t prev_rpm;
    uint16_t rpm_run;               /* consecutive ticks RPM held settled */
    /* self-tracked spin-up grace + fault-override edge */
    uint8_t  prev_pwm_low;
    uint16_t spinup_remaining;
    uint8_t  prev_override;
    /* per-baseline-point accumulators */
    int32_t  ema_x256[THERMAL_MAX_FAN_HEALTH_POINTS];  /* EMA of delta_pct, x256 */
    uint16_t sample_count[THERMAL_MAX_FAN_HEALTH_POINTS];
    /* last published outputs (read by the telemetry dispatcher) */
    int16_t  health_delta_pct;
    uint8_t  severity;
    uint8_t  confidence;
} thermal_fan_health_state_t;

void thermal_fan_health_reset(thermal_fan_health_state_t *s);

/* Process one control tick for one actuator.
 *   applied_pwm    -- the previous tick's final applied duty_0_255
 *                     (the duty that produced this tach sample)
 *   tach_rpm       -- measured RPM
 *   tach_valid     -- 0 -> sample skipped
 *   slew_limited   -- the previous tick's slew_limited flag
 *   fault_override -- this tick's force-max / shutdown override active
 *   spinup_ticks   -- length of the spin-up grace window, in ticks
 * Updates `s` in place. Never calls a callback, never writes an
 * actuator command. */
void thermal_fan_health_step(thermal_fan_health_state_t *s,
                             const thermal_fan_health_cfg_t *cfg,
                             uint8_t  applied_pwm,
                             uint16_t tach_rpm,
                             uint8_t  tach_valid,
                             uint8_t  slew_limited,
                             uint8_t  fault_override,
                             uint16_t spinup_ticks);

#endif /* THERMALCORE_ENABLE_FAN_HEALTH */
#endif /* THERMAL_FAN_HEALTH_H */
