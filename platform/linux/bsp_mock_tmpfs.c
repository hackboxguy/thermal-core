/* platform/linux/bsp_mock_tmpfs.c
 *
 * Stage 9 9b — Linux file-I/O BSP implementation.
 *
 * Reads ASCII-integer files (hwmon convention) into thermal_sample_t
 * values and writes ASCII-integer files for actuator PWM.  Errors
 * surface as valid=0 samples; the core's fault detectors then take
 * over per PRD §4.7.
 */
#include "bsp_mock_tmpfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* =============================================================== */
/* Internal helpers                                                  */
/* =============================================================== */

/* Read up to 31 bytes of an ASCII integer from `path`, parse with
 * strtol, store in *out.  Returns 0 on success, nonzero on any
 * failure (missing file, empty file, non-numeric content, etc.). */
static int read_int_file(const char *path, long *out)
{
    if (!path || path[0] == '\0') return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char buf[32];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';

    char *endp;
    errno = 0;
    long v = strtol(buf, &endp, 10);
    if (endp == buf) return -1;
    if (errno == ERANGE) return -1;
    /* Trailing whitespace / newline is allowed and ignored. */
    *out = v;
    return 0;
}

/* Write an unsigned decimal followed by '\n' to `path`.  Used for
 * actuator PWM writes (hwmon expects "%u\n"). */
static int write_uint_file(const char *path, unsigned int value)
{
    if (!path || path[0] == '\0') return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int rc = fprintf(f, "%u\n", value);
    int close_rc = fclose(f);
    if (rc < 0 || close_rc != 0) return -1;
    return 0;
}

/* Fill a thermal_sample_t with id+kind+ts and the read result. */
static void fill_sample(thermal_sample_t *out,
                        uint16_t id,
                        uint8_t kind,
                        uint32_t now_ms,
                        int read_ok,
                        long value)
{
    out->id = id;
    out->kind = kind;
    out->sample_ts_ms = now_ms;
    out->quality = 0;
    if (read_ok) {
        out->valid = 1;
        out->value = (int32_t)value;
    } else {
        out->valid = 0;
        out->value = 0;
    }
}

/* =============================================================== */
/* Public read API                                                   */
/* =============================================================== */

int bsp_mock_tmpfs_read_sensor(const thermalcored_runtime_cfg_t *runtime,
                               const thermal_config_t *cfg,
                               uint8_t slot,
                               uint32_t now_ms,
                               thermal_sample_t *out)
{
    if (!runtime || !cfg || !out) return -1;
    if (slot >= cfg->sensor_count) return -1;

    long v = 0;
    int rc = read_int_file(runtime->sensors[slot].source, &v);
    fill_sample(out, cfg->sensors[slot].id, THERMAL_SAMPLE_TEMP_MC,
                now_ms, rc == 0, v);
    return 0;
}

int bsp_mock_tmpfs_read_tach(const thermalcored_runtime_cfg_t *runtime,
                             const thermal_config_t *cfg,
                             uint8_t slot,
                             uint32_t now_ms,
                             thermal_sample_t *out)
{
    if (!runtime || !cfg || !out) return -1;
    if (slot >= cfg->actuator_count) return -1;

    long v = 0;
    int rc = read_int_file(runtime->actuators[slot].tach, &v);
    fill_sample(out, cfg->actuators[slot].id, THERMAL_SAMPLE_TACH_RPM,
                now_ms, rc == 0, v);
    return 0;
}

int bsp_mock_tmpfs_read_context(const thermalcored_runtime_cfg_t *runtime,
                                const thermal_config_t *cfg,
                                uint8_t slot,
                                uint32_t now_ms,
                                thermal_sample_t *out)
{
    if (!runtime || !cfg || !out) return -1;
    if (slot >= cfg->context_count) return -1;

    long v = 0;
    int rc = read_int_file(runtime->contexts[slot].source, &v);
    fill_sample(out, cfg->contexts[slot].id, THERMAL_SAMPLE_CONTEXT_I32,
                now_ms, rc == 0, v);
    return 0;
}

/* =============================================================== */
/* Public write API                                                  */
/* =============================================================== */

int bsp_mock_tmpfs_write_actuator(const thermalcored_runtime_cfg_t *runtime,
                                  const thermal_config_t *cfg,
                                  uint8_t slot,
                                  uint8_t duty_0_255)
{
    if (!runtime || !cfg) return -1;
    if (slot >= cfg->actuator_count) return -1;
    return write_uint_file(runtime->actuators[slot].pwm,
                           (unsigned int)duty_0_255);
}

/* =============================================================== */
/* Per-tick aggregators                                              */
/* =============================================================== */

int bsp_mock_tmpfs_build_snapshot(const thermalcored_runtime_cfg_t *runtime,
                                  const thermal_config_t *cfg,
                                  uint32_t now_ms,
                                  thermal_sample_t *samples,
                                  uint8_t samples_max,
                                  thermal_input_snapshot_t *snap_out)
{
    if (!runtime || !cfg || !samples || !snap_out) return -1;

    uint8_t need = cfg->sensor_count + cfg->actuator_count + cfg->context_count;
    if (need > samples_max) return -1;

    uint8_t n = 0;
    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        if (bsp_mock_tmpfs_read_sensor(runtime, cfg, i, now_ms,
                                       &samples[n]) != 0) {
            return -1;
        }
        n++;
    }
    for (uint8_t i = 0; i < cfg->actuator_count; i++) {
        if (bsp_mock_tmpfs_read_tach(runtime, cfg, i, now_ms,
                                     &samples[n]) != 0) {
            return -1;
        }
        n++;
    }
    for (uint8_t i = 0; i < cfg->context_count; i++) {
        if (bsp_mock_tmpfs_read_context(runtime, cfg, i, now_ms,
                                        &samples[n]) != 0) {
            return -1;
        }
        n++;
    }

    snap_out->now_ms = now_ms;
    snap_out->samples = samples;
    snap_out->sample_count = n;
    return 0;
}

/* Look up which slot owns the given actuator id; returns -1 on miss. */
static int actuator_slot_for_id(const thermal_config_t *cfg, uint16_t id)
{
    for (uint8_t i = 0; i < cfg->actuator_count; i++) {
        if (cfg->actuators[i].id == id) return (int)i;
    }
    return -1;
}

int bsp_mock_tmpfs_write_frame(const thermalcored_runtime_cfg_t *runtime,
                               const thermal_config_t *cfg,
                               const thermal_output_frame_t *frame)
{
    if (!runtime || !cfg || !frame) return -1;

    int worst = 0;
    for (uint8_t i = 0; i < frame->actuator_cmd_count; i++) {
        const thermal_actuator_cmd_t *cmd = &frame->actuator_cmds[i];
        int slot = actuator_slot_for_id(cfg, cmd->actuator_id);
        if (slot < 0) { worst = -1; continue; }
        if (bsp_mock_tmpfs_write_actuator(runtime, cfg, (uint8_t)slot,
                                           cmd->duty_0_255) != 0) {
            worst = -1;
        }
    }
    return worst;
}
