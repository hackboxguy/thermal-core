/* support/thermal_config_hash.h
 *
 * Stage 9 9d — canonical thermal_config_t hash (PRD §7.6).
 *
 * Walks thermal_config_t field-by-field in a locked order, encoding
 * scalars little-endian, names as NUL-terminated NAME_MAX-byte blocks,
 * and arrays to compile-time maximum (unused trailing slots zero-
 * filled).  The bytes are fed to SHA-256.  Result is a 32-byte
 * digest that is stable across struct-padding choices, sibling-cfg
 * structs, and embedded targets that don't link the JSON loader.
 *
 * Never use `sha256(&cfg, sizeof cfg)` — struct padding makes that
 * non-deterministic and breaks the round-trip with json2static.
 * The padding-poison test in test/unit/test_config_hash.c pins this.
 *
 * `support/` is portable C99 with no platform / heap deps; depends
 * only on `core/` types.
 */
#ifndef THERMAL_SUPPORT_CONFIG_HASH_H
#define THERMAL_SUPPORT_CONFIG_HASH_H

#include <stdint.h>
#include "thermal_core.h"
#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compute the canonical SHA-256 of `cfg`.  `out` receives the
 * 32-byte digest.  Both parameters must be non-NULL. */
void thermal_config_hash(const thermal_config_t *cfg,
                         uint8_t out[SHA256_DIGEST_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_SUPPORT_CONFIG_HASH_H */
