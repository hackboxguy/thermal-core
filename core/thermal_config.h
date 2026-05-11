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

#endif /* THERMAL_CONFIG_H */
