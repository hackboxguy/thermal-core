/* test/unit/test_fan_health_roundtrip.c
 *
 * Stage 17 17b -- JSON loader <-> json2static round-trip for a config
 * that carries a per-actuator fan_health block (PRD Appendix C).
 *
 * Parallels test_json2static_roundtrip.c, but exercises the fan-health
 * config path: if json2static.py or config_jsmn.c drops, mis-ranges, or
 * disagrees on any fan_health field, the canonical hashes diverge.
 *
 * Build wiring (Makefile):
 *   - tools/json2static.py --enable-fan-health is run on
 *     configs/fan-health-demo.json -> build/test/generated/
 *     fan_health_static.c, declaring `const thermal_config_t
 *     G_THERMAL_CFG`.
 *   - That file links alongside this driver, the JSON loader, the
 *     canonical hash, and libthermal_core.a.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "thermal_core.h"
#include "thermal_config.h"
#include "config_jsmn.h"
#include "sha256.h"
#include "thermal_config_hash.h"

extern const thermal_config_t G_THERMAL_CFG;

static char *read_file_or_die(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "FAIL: cannot open '%s'\n", path); exit(1); }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); exit(1); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); exit(1); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); exit(1); }
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(f); exit(1); }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); exit(1); }
    *out_len = n;
    return buf;
}

static void hex_print(const uint8_t *buf, size_t n, char *out, size_t out_sz)
{
    size_t off = 0;
    for (size_t i = 0; i < n && off + 3 < out_sz; i++) {
        off += (size_t)snprintf(out + off, out_sz - off, "%02x", buf[i]);
    }
}

TEST_CASE(fan_health_roundtrip) {
    size_t n;
    char *json = read_file_or_die("configs/fan-health-demo.json", &n);
    thermal_config_t cfg_json;
    char err[256];
    thermal_status_t s = thermal_config_jsmn_parse(json, n, &cfg_json, NULL,
                                                   err, sizeof(err));
    if (s != THERMAL_OK) {
        fprintf(stderr, "loader failed: status=%d err='%s'\n", (int)s, err);
        exit(1);
    }
    free(json);

    /* The fan_health block must have survived the loader. */
    EXPECT_EQ(cfg_json.actuator_count, 1);
    EXPECT_EQ(cfg_json.fan_health[0].enable, 1);
    EXPECT_EQ(cfg_json.fan_health[0].baseline_count, 6);

    uint8_t h_json[32], h_static[32];
    thermal_config_hash(&cfg_json,      h_json);
    thermal_config_hash(&G_THERMAL_CFG, h_static);

    if (memcmp(h_json, h_static, 32) != 0) {
        char a[80], b[80];
        hex_print(h_json,   32, a, sizeof(a));
        hex_print(h_static, 32, b, sizeof(b));
        fprintf(stderr,
                "fan-health round-trip mismatch:\n"
                "  loader hash: %s\n"
                "  static hash: %s\n", a, b);
        exit(1);
    }
}
