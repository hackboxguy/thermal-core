/* test/unit/test_bsp_mock_tmpfs.c
 *
 * Verify the file-I/O BSP against a tmpfs root.  Uses mkdtemp to
 * create a unique scratch directory, plants files, populates a
 * thermalcored_runtime_cfg_t pointing into the directory, and runs
 * seven scenarios: positive sensor read, missing-file, malformed,
 * empty-tach, actuator write, snapshot build, full output frame.
 */
/* mkdtemp lives in POSIX.1-2008; opt into the feature set under
 * -std=c99 -pedantic. */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "harness.h"
#include "thermal_core.h"
#include "thermal_config.h"
#include "thermal_types.h"
#include "runtime_cfg.h"
#include "bsp_mock_tmpfs.h"

/* === Filesystem scaffolding ============================================ */

static void write_text_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "FAIL: cannot create '%s'\n", path);
        exit(1);
    }
    if (fputs(content, f) < 0) { fclose(f); exit(1); }
    fclose(f);
}

static void read_text_file(const char *path, char *buf, size_t buf_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(buf, buf_sz, "<missing>");
        return;
    }
    size_t n = fread(buf, 1, buf_sz - 1, f);
    fclose(f);
    buf[n] = '\0';
}

static int rm_rf(const char *root)
{
    DIR *d = opendir(root);
    if (!d) return -1;
    struct dirent *ent;
    char child[1024];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0) continue;
        if (strcmp(ent->d_name, "..") == 0) continue;
        snprintf(child, sizeof(child), "%s/%s", root, ent->d_name);
        unlink(child);
    }
    closedir(d);
    rmdir(root);
    return 0;
}

/* === Helper to fill a minimal runtime cfg pointing into root ========= */

static void make_runtime_paths(thermalcored_runtime_cfg_t *runtime,
                                const char *root)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->sensor_count = 1;
    snprintf(runtime->sensors[0].source,
             sizeof(runtime->sensors[0].source),
             "%s/temp1_input", root);

    runtime->actuator_count = 1;
    snprintf(runtime->actuators[0].pwm,
             sizeof(runtime->actuators[0].pwm),
             "%s/pwm1", root);
    snprintf(runtime->actuators[0].tach,
             sizeof(runtime->actuators[0].tach),
             "%s/fan1_input", root);

    runtime->context_count = 1;
    snprintf(runtime->contexts[0].source,
             sizeof(runtime->contexts[0].source),
             "%s/speed", root);
}

static void make_cfg(thermal_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->sensor_count = 1;
    cfg->sensors[0].id = 7;     /* deliberately not 0 to exercise id != slot */
    cfg->actuator_count = 1;
    cfg->actuators[0].id = 9;
    cfg->context_count = 1;
    cfg->contexts[0].id = 11;
}

/* === Test ============================================================ */

TEST_CASE(bsp_mock_tmpfs) {
    char root[64];
    snprintf(root, sizeof(root), "/tmp/thermal_bsp_test_XXXXXX");
    if (mkdtemp(root) == NULL) {
        fprintf(stderr, "FAIL: mkdtemp\n");
        exit(1);
    }

    thermal_config_t           cfg;
    thermalcored_runtime_cfg_t runtime;
    make_cfg(&cfg);
    make_runtime_paths(&runtime, root);

    char p_temp[256], p_pwm[256], p_tach[256], p_speed[256];
    snprintf(p_temp,  sizeof(p_temp),  "%s/temp1_input", root);
    snprintf(p_pwm,   sizeof(p_pwm),   "%s/pwm1",        root);
    snprintf(p_tach,  sizeof(p_tach),  "%s/fan1_input",  root);
    snprintf(p_speed, sizeof(p_speed), "%s/speed",       root);

    /* === Scenario 1: positive sensor read ============================ */
    write_text_file(p_temp, "78000\n");
    {
        thermal_sample_t s;
        EXPECT_EQ(bsp_mock_tmpfs_read_sensor(&runtime, &cfg, 0, 42, &s), 0);
        EXPECT_EQ(s.id, 7);
        EXPECT_EQ(s.kind, THERMAL_SAMPLE_TEMP_MC);
        EXPECT_EQ(s.valid, 1);
        EXPECT_EQ(s.value, 78000);
        EXPECT_EQ(s.sample_ts_ms, 42);
    }

    /* === Scenario 2: missing file -> valid=0 ========================= */
    unlink(p_temp);
    {
        thermal_sample_t s;
        EXPECT_EQ(bsp_mock_tmpfs_read_sensor(&runtime, &cfg, 0, 100, &s), 0);
        EXPECT_EQ(s.valid, 0);
        EXPECT_EQ(s.value, 0);
    }

    /* === Scenario 3: malformed content -> valid=0 ==================== */
    write_text_file(p_temp, "not a number");
    {
        thermal_sample_t s;
        EXPECT_EQ(bsp_mock_tmpfs_read_sensor(&runtime, &cfg, 0, 100, &s), 0);
        EXPECT_EQ(s.valid, 0);
    }

    /* === Scenario 4: empty tach path -> valid=0 ====================== */
    {
        thermalcored_runtime_cfg_t r2 = runtime;
        r2.actuators[0].tach[0] = '\0';
        thermal_sample_t s;
        EXPECT_EQ(bsp_mock_tmpfs_read_tach(&r2, &cfg, 0, 100, &s), 0);
        EXPECT_EQ(s.valid, 0);
    }

    /* === Scenario 5: actuator write =================================== */
    {
        EXPECT_EQ(bsp_mock_tmpfs_write_actuator(&runtime, &cfg, 0, 160), 0);
        char content[64];
        read_text_file(p_pwm, content, sizeof(content));
        EXPECT_EQ(strcmp(content, "160\n"), 0);
    }

    /* === Scenario 6: build_snapshot returns all three samples ======== */
    write_text_file(p_temp,  "70000\n");
    write_text_file(p_tach,  "1500\n");
    write_text_file(p_speed, "120\n");
    {
        thermal_sample_t samples[8];
        thermal_input_snapshot_t snap;
        EXPECT_EQ(bsp_mock_tmpfs_build_snapshot(&runtime, &cfg, 99,
                                                samples, 8, &snap), 0);
        EXPECT_EQ(snap.now_ms, 99);
        EXPECT_EQ(snap.sample_count, 3);
        /* Order: sensors, actuators (tach), contexts. */
        EXPECT_EQ(snap.samples[0].id,    7);
        EXPECT_EQ(snap.samples[0].kind,  THERMAL_SAMPLE_TEMP_MC);
        EXPECT_EQ(snap.samples[0].value, 70000);
        EXPECT_EQ(snap.samples[1].id,    9);
        EXPECT_EQ(snap.samples[1].kind,  THERMAL_SAMPLE_TACH_RPM);
        EXPECT_EQ(snap.samples[1].value, 1500);
        EXPECT_EQ(snap.samples[2].id,    11);
        EXPECT_EQ(snap.samples[2].kind,  THERMAL_SAMPLE_CONTEXT_I32);
        EXPECT_EQ(snap.samples[2].value, 120);
    }

    /* === Scenario 7: write_frame routes by actuator_id ============== */
    {
        thermal_output_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.actuator_cmd_count = 1;
        frame.actuator_cmds[0].actuator_id = 9;   /* matches cfg.actuators[0].id */
        frame.actuator_cmds[0].duty_0_255  = 200;
        EXPECT_EQ(bsp_mock_tmpfs_write_frame(&runtime, &cfg, &frame), 0);
        char content[64];
        read_text_file(p_pwm, content, sizeof(content));
        EXPECT_EQ(strcmp(content, "200\n"), 0);
    }

    rm_rf(root);
}
