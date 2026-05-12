/* support/sha256.h
 *
 * Stage 9 9d — minimal FIPS 180-4 SHA-256.
 *
 * Plain C99 implementation written from spec; no allocation, no
 * syscalls, no platform deps.  Used by support/thermal_config_hash.c
 * to derive a stable digest over the canonical encoding of
 * thermal_config_t (PRD §7.6).
 *
 * Self-tested at unit-test time against the FIPS test vectors.
 */
#ifndef THERMAL_SUPPORT_SHA256_H
#define THERMAL_SUPPORT_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_LEN  32u

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buffer[64];
    uint32_t buffer_len;
} sha256_ctx_t;

void sha256_init  (sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_final (sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_SUPPORT_SHA256_H */
