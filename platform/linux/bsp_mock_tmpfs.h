/* platform/linux/bsp_mock_tmpfs.h
 *
 * Stage 9 9b — Linux file-I/O BSP.
 *
 * Bridges the core's pure-data sample/command interface to hwmon-
 * style sysfs paths.  Despite the name, the code works against any
 * filesystem path: a real `/sys/class/hwmon/...` tree in production
 * and a tmpfs `/tmp/.../` tree for unit/scenario/smoke tests.  Only
 * the path strings (in thermalcored_runtime_cfg_t) change.
 *
 * Sensor / tach / context reads parse the file as an ASCII integer
 * (typical hwmon convention) and produce a thermal_sample_t with
 * the right kind and id.  On any open/read/parse failure, the
 * sample is emitted with valid=0 — the core's fault detectors
 * handle that case (stuck_sensor / stale_context).
 *
 * Actuator writes format the duty value as decimal followed by a
 * newline (`"%u\n"`) and write it to runtime->actuators[slot].pwm.
 */
#ifndef THERMAL_BSP_MOCK_TMPFS_H
#define THERMAL_BSP_MOCK_TMPFS_H

#include <stdint.h>
#include "thermal_core.h"
#include "thermal_types.h"
#include "runtime_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read one sensor file at runtime->sensors[slot].source.  Emits a
 * sample with id=cfg->sensors[slot].id, kind=TEMP_MC, sample_ts_ms
 * =now_ms.  On filesystem error, missing/empty file, or parse
 * failure: out->valid=0 and out->value=0.  Returns 0 on success
 * (incl. valid=0 path), nonzero on caller-error (NULL args, slot
 * out of range). */
int bsp_mock_tmpfs_read_sensor(const thermalcored_runtime_cfg_t *runtime,
                               const thermal_config_t *cfg,
                               uint8_t slot,
                               uint32_t now_ms,
                               thermal_sample_t *out);

/* Read one tach file at runtime->actuators[slot].tach (kind=
 * TACH_RPM, value=raw integer read from the file).  Empty tach
 * path → valid=0. */
int bsp_mock_tmpfs_read_tach(const thermalcored_runtime_cfg_t *runtime,
                             const thermal_config_t *cfg,
                             uint8_t slot,
                             uint32_t now_ms,
                             thermal_sample_t *out);

/* Read one context file at runtime->contexts[slot].source
 * (kind=CONTEXT_I32). */
int bsp_mock_tmpfs_read_context(const thermalcored_runtime_cfg_t *runtime,
                                const thermal_config_t *cfg,
                                uint8_t slot,
                                uint32_t now_ms,
                                thermal_sample_t *out);

/* Write actuator duty (0..255) to runtime->actuators[slot].pwm.
 * Returns 0 on success, nonzero on filesystem failure. */
int bsp_mock_tmpfs_write_actuator(const thermalcored_runtime_cfg_t *runtime,
                                  const thermal_config_t *cfg,
                                  uint8_t slot,
                                  uint8_t duty_0_255);

/* Build an input snapshot from current state of every configured
 * sensor + tach + context.  Caller owns the samples[] backing
 * buffer (sized for at least sensor_count + actuator_count +
 * context_count entries).  Samples are appended in that order,
 * with invalid reads kept (valid=0) so the core sees them.
 *
 * Returns 0 on success.  Nonzero indicates samples_max was too
 * small for the configured slot counts; partial state in snap_out
 * is then undefined. */
int bsp_mock_tmpfs_build_snapshot(const thermalcored_runtime_cfg_t *runtime,
                                  const thermal_config_t *cfg,
                                  uint32_t now_ms,
                                  thermal_sample_t *samples,
                                  uint8_t samples_max,
                                  thermal_input_snapshot_t *snap_out);

/* Walk frame->actuator_cmds[] and write each duty to its sysfs
 * path.  Returns 0 only if every write succeeded. */
int bsp_mock_tmpfs_write_frame(const thermalcored_runtime_cfg_t *runtime,
                               const thermal_config_t *cfg,
                               const thermal_output_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_BSP_MOCK_TMPFS_H */
