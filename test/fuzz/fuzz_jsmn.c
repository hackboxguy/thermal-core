/* test/fuzz/fuzz_jsmn.c
 *
 * libFuzzer harness around thermal_config_jsmn_parse.  Built only
 * by the `fuzz-json` Makefile target (requires clang).  Seed corpus
 * lives in test/fuzz/seeds/.
 *
 * The harness must never crash regardless of input — that's the
 * contract being verified.  Any crash is a bug in the loader.
 */
#include <stddef.h>
#include <stdint.h>

#include "config_jsmn.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    thermal_config_t cfg;
    char err[64];
    (void)thermal_config_jsmn_parse((const char *)data, size,
                                    &cfg, NULL, err, sizeof(err));
    return 0;
}
