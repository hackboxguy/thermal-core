/* core/thermal_config.h
 *
 * Compile-time maxima for thermal-core. Pinned by PRD §4.2.
 * All sizes are static — `core/` does not allocate after init.
 * Re-tunable at build, but bound at compile time.
 */
#ifndef THERMAL_CONFIG_H
#define THERMAL_CONFIG_H

#define THERMAL_MAX_ZONES                  4
#define THERMAL_MAX_SENSORS                8
#define THERMAL_MAX_ACTUATORS              2
#define THERMAL_MAX_CONTEXT_SIGNALS        4
#define THERMAL_MAX_TRIPS_PER_ZONE         4
#define THERMAL_MAX_MODIFIERS              2
#define THERMAL_MAX_FAULTS                24
#define THERMAL_MAX_SAMPLES_PER_SNAPSHOT  16
#define THERMAL_MAX_SENSORS_PER_ZONE       4
#define THERMAL_MAX_ACTUATORS_PER_ZONE     2
#define THERMAL_MAX_COOLING_STATES         5
#define THERMAL_MAX_CURVE_POINTS           8
#define THERMAL_MAX_TELEMETRY_SIGNALS    128
#define THERMAL_NAME_MAX                  24

/* Reserved bytes for the public thermal_core_t. The internal struct
 * (core/thermal_core.c) grows within this budget across Stages 2-7;
 * raising this is a deliberate PR with rationale. */
#define THERMAL_CORE_T_RESERVED_BYTES   4096

/* Q16.16 fixed-point unity. Used for IIR filter coefficients, PID
 * gains, sensor weights, and curve interpolation throughout core/. */
#define Q16_ONE                  0x00010000

#endif /* THERMAL_CONFIG_H */
