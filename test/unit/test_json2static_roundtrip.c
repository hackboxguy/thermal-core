/* test/unit/test_json2static_roundtrip.c
 *
 * Stage 9 9d — JSON loader ↔ json2static round-trip via canonical hash.
 *
 * Build wiring:
 *   - Makefile runs tools/json2static.py on
 *     configs/minimal-1zone-1fan.json and writes the result to
 *     build/test/generated/minimal_static.c.
 *   - That generated file declares `const thermal_config_t G_THERMAL_CFG`.
 *   - Both this file and the generated .c are linked into the test
 *     binary, alongside the JSON loader, the canonical hash, and
 *     libthermal_core.a.
 *
 * Runtime:
 *   1. Read configs/minimal-1zone-1fan.json, parse via
 *      thermal_config_jsmn_parse → cfg_json.
 *   2. Hash cfg_json → h_json.
 *   3. Hash the static G_THERMAL_CFG → h_static.
 *   4. Assert h_json == h_static (byte-equal).
 *
 * If either codegen or loader drops a field, mis-encodes an enum,
 * or expands a wildcard incorrectly, the hashes diverge.
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

TEST_CASE(json2static_roundtrip) {
    /* 1. Load JSON via the C loader. */
    size_t n;
    char *json = read_file_or_die("configs/minimal-1zone-1fan.json", &n);
    thermal_config_t cfg_json;
    char err[256];
    thermal_status_t s = thermal_config_jsmn_parse(json, n, &cfg_json, NULL,
                                                    err, sizeof(err));
    if (s != THERMAL_OK) {
        fprintf(stderr, "loader failed: status=%d err='%s'\n", (int)s, err);
        exit(1);
    }
    free(json);

    /* 2. Hash both. */
    uint8_t h_json[32], h_static[32];
    thermal_config_hash(&cfg_json,     h_json);
    thermal_config_hash(&G_THERMAL_CFG, h_static);

    /* 3. Compare. */
    if (memcmp(h_json, h_static, 32) != 0) {
        char a[80], b[80];
        hex_print(h_json,   32, a, sizeof(a));
        hex_print(h_static, 32, b, sizeof(b));
        fprintf(stderr,
                "round-trip mismatch:\n"
                "  loader hash: %s\n"
                "  static hash: %s\n", a, b);
        exit(1);
    }
}
