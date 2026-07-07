/* core/thermal_config.h
 *
 * Compile-time maxima for thermal-core. Pinned by PRD §4.2.
 * All sizes are static — `core/` does not allocate after init.
 * Re-tunable at build, but bound at compile time.
 *
 * Each maximum is #ifndef-guarded so a build profile — force-included
 * ahead of this header with -include (see core/thermal_profile_tiny.h)
 * — can override it for a constrained target without editing this
 * file. The default build sets none of them and gets the values here.
 */
#ifndef THERMAL_CONFIG_H
#define THERMAL_CONFIG_H

#ifndef THERMAL_MAX_ZONES
#define THERMAL_MAX_ZONES                  4
#endif
#ifndef THERMAL_MAX_SENSORS
#define THERMAL_MAX_SENSORS                8
#endif
#ifndef THERMAL_MAX_ACTUATORS
#define THERMAL_MAX_ACTUATORS              2
#endif
#ifndef THERMAL_MAX_CONTEXT_SIGNALS
#define THERMAL_MAX_CONTEXT_SIGNALS        4
#endif
#ifndef THERMAL_MAX_TRIPS_PER_ZONE
#define THERMAL_MAX_TRIPS_PER_ZONE         4
#endif
#ifndef THERMAL_MAX_MODIFIERS
#define THERMAL_MAX_MODIFIERS              2
#endif
#ifndef THERMAL_MAX_FAULTS
#define THERMAL_MAX_FAULTS                24
#endif
#ifndef THERMAL_MAX_SAMPLES_PER_SNAPSHOT
#define THERMAL_MAX_SAMPLES_PER_SNAPSHOT  16
#endif
#ifndef THERMAL_MAX_SENSORS_PER_ZONE
#define THERMAL_MAX_SENSORS_PER_ZONE       4
#endif
#ifndef THERMAL_MAX_ACTUATORS_PER_ZONE
#define THERMAL_MAX_ACTUATORS_PER_ZONE     2
#endif
#ifndef THERMAL_MAX_COOLING_STATES
#define THERMAL_MAX_COOLING_STATES         5
#endif
#ifndef THERMAL_MAX_CURVE_POINTS
#define THERMAL_MAX_CURVE_POINTS           8
#endif
/* PWM-to-RPM baseline points for the post-v1 fan-health detector
 * (PRD Appendix C, Stage 17). Sized to match THERMAL_MAX_CURVE_POINTS;
 * the detector reuses thermal_curve.c interpolation. */
#ifndef THERMAL_MAX_FAN_HEALTH_POINTS
#define THERMAL_MAX_FAN_HEALTH_POINTS      8
#endif
#ifndef THERMAL_MAX_TELEMETRY_SIGNALS
#define THERMAL_MAX_TELEMETRY_SIGNALS    128
#endif
#ifndef THERMAL_NAME_MAX
#define THERMAL_NAME_MAX                  24
#endif

/* Governor feature gates. PID is available by default for host and
 * larger MCU builds; constrained profiles may compile it out while
 * preserving the public enum and config layout. */
#ifndef THERMALCORE_ENABLE_PID
#define THERMALCORE_ENABLE_PID             1
#endif

/* Reserved bytes for the public thermal_core_t. The internal struct
 * (core/thermal_core.c) grows within this budget across Stages 2-7;
 * raising this is a deliberate PR with rationale. A constrained
 * profile may shrink it (the thermal_core_t_fits assertion in
 * thermal_core.c keeps it honest against the internal struct). */
#ifndef THERMAL_CORE_T_RESERVED_BYTES
#define THERMAL_CORE_T_RESERVED_BYTES   4096
#endif

/* Q16.16 fixed-point unity. Used for IIR filter coefficients, PID
 * gains, sensor weights, and curve interpolation throughout core/.
 * A mathematical constant, not a profile-tunable maximum. */
#define Q16_ONE                  0x00010000

/* Maximum persist_ticks window the runaway fault detector can track.
 * The detector keeps a per-instance ring buffer of (temp, pwm) sized
 * to this constant; validate_config rejects runaway.persist_ticks
 * greater than this value (Stage 6 rule 32). */
#ifndef THERMAL_FAULT_RUNAWAY_WINDOW_MAX
#define THERMAL_FAULT_RUNAWAY_WINDOW_MAX  64
#endif

#endif /* THERMAL_CONFIG_H */
