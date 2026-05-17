/* core/thermal_profile_tiny.h
 *
 * Tiny build profile for severely flash- and RAM-constrained MCUs
 * (PRD Appendix D.3). Force-include this header ahead of everything
 * with the compiler's -include flag; it pre-#defines the
 * THERMAL_MAX_* maxima so the #ifndef guards in thermal_config.h
 * leave these values in place instead of the defaults.
 *
 * Target-agnostic: any constrained-MCU port reuses it. First user
 * is platform/ch32v003/ (WCH CH32V003, 16 KB flash / 2 KB SRAM).
 *
 * Rationale for each value:
 *  - zones/sensors/actuators = 1: a constrained node is single-zone.
 *  - context signals / modifiers = 1: no vehicle context, but C99
 *    -pedantic forbids zero-length arrays, so the floor is 1.
 *  - trips-per-zone / cooling-states / curve-points: kept at the
 *    default — one zone still runs the full trip staircase.
 *  - faults = 4, samples-per-snapshot = 2: PRD D.3.
 *  - telemetry signals / runaway window: shrunk; a headless
 *    single-zone regulator needs only a handful.
 *  - reserved bytes = 1024: the tiny thermal_core_internal_t is
 *    ~760 B (PRD D.3); thermal_core.c's thermal_core_t_fits
 *    assertion verifies the internal struct fits this budget.
 */
#ifndef THERMAL_PROFILE_TINY_H
#define THERMAL_PROFILE_TINY_H

#define THERMAL_MAX_ZONES                  1
#define THERMAL_MAX_SENSORS                1
#define THERMAL_MAX_ACTUATORS              1
#define THERMAL_MAX_CONTEXT_SIGNALS        1
#define THERMAL_MAX_TRIPS_PER_ZONE         4
#define THERMAL_MAX_MODIFIERS              1
#define THERMAL_MAX_FAULTS                 4
#define THERMAL_MAX_SAMPLES_PER_SNAPSHOT   2
#define THERMAL_MAX_SENSORS_PER_ZONE       1
#define THERMAL_MAX_ACTUATORS_PER_ZONE     1
#define THERMAL_MAX_COOLING_STATES         5
#define THERMAL_MAX_CURVE_POINTS           8
#define THERMAL_MAX_TELEMETRY_SIGNALS      8
#define THERMAL_NAME_MAX                  24

#define THERMAL_CORE_T_RESERVED_BYTES   1024

#define THERMAL_FAULT_RUNAWAY_WINDOW_MAX  16

#endif /* THERMAL_PROFILE_TINY_H */
