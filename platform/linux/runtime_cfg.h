/* platform/linux/runtime_cfg.h
 *
 * Stage 9 9b — platform-only (BSP-owned) cfg struct.
 *
 * Sibling to thermal_config_t. The JSON loader (config_jsmn) parses
 * the PRD §5.1 schema into two structs: the deterministic CORE half
 * goes into thermal_config_t (consumed by core/), the Linux-specific
 * half (file paths, transports, listen specs) goes into
 * thermalcored_runtime_cfg_t (consumed by the BSPs and the daemon).
 *
 * Slot indexing mirrors the CORE struct: runtime->sensors[i]
 * describes how to source the value for cfg->sensors[i] etc.  Slot
 * counts are filled by the loader to match cfg->{sensor,actuator,
 * context}_count.
 *
 * Header-only in 9b — no helpers needed yet.  9c may add a
 * runtime_cfg_clear() if it grows.
 */
#ifndef THERMAL_RUNTIME_CFG_H
#define THERMAL_RUNTIME_CFG_H

#include <stdint.h>
#include "thermal_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length (incl. trailing NUL) of any single filesystem path
 * or transport URI stored in the runtime cfg.  Sized to comfortably
 * fit typical hwmon paths (~30 chars) and UDP/UDS specs (~64 chars)
 * with headroom.  Bumping this is a deliberate change because the
 * struct grows linearly with it. */
#define THERMAL_PATH_MAX 128

typedef struct {
    char source[THERMAL_PATH_MAX]; /* hwmon temp*_input or tmpfs file */
} runtime_sensor_cfg_t;

typedef struct {
    char     pwm[THERMAL_PATH_MAX];  /* hwmon pwm* path */
    char     tach[THERMAL_PATH_MAX]; /* hwmon fan*_input path; "" = none */
    uint32_t pwm_freq_hz;            /* informational; written to pwm*_freq if BSP supports it */
    uint16_t tach_pulses_per_rev;    /* informational; v1 reads tach as RPM directly */
} runtime_actuator_cfg_t;

typedef struct {
    char    source[THERMAL_PATH_MAX]; /* file path for the context value */
    int32_t fail_safe_value;          /* used when fail_safe == ASSUME_VALUE */
} runtime_context_cfg_t;

typedef struct {
    char    telemetry_transport[THERMAL_PATH_MAX]; /* e.g. "udp:127.0.0.1:9000" */
    char    control_listen[THERMAL_PATH_MAX];      /* socket spec (Stage 10) */
    uint8_t control_enable;   /* PRD §7.3 line 945: listener off unless true */
} runtime_global_cfg_t;

typedef struct {
    runtime_sensor_cfg_t   sensors[THERMAL_MAX_SENSORS];
    uint8_t                sensor_count;
    runtime_actuator_cfg_t actuators[THERMAL_MAX_ACTUATORS];
    uint8_t                actuator_count;
    runtime_context_cfg_t  contexts[THERMAL_MAX_CONTEXT_SIGNALS];
    uint8_t                context_count;
    runtime_global_cfg_t   global;
} thermalcored_runtime_cfg_t;

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_RUNTIME_CFG_H */
