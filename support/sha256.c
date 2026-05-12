/* support/sha256.c
 *
 * SHA-256 (FIPS PUB 180-4) plain C99 implementation.
 * Written from spec; no allocation, no syscalls.
 */
#include "sha256.h"

#include <string.h>

/* === FIPS 180-4 constants ============================================ */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

/* === Helpers ========================================================= */

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static uint32_t big_sigma0(uint32_t x) { return rotr32(x,  2) ^ rotr32(x, 13) ^ rotr32(x, 22); }
static uint32_t big_sigma1(uint32_t x) { return rotr32(x,  6) ^ rotr32(x, 11) ^ rotr32(x, 25); }
static uint32_t lil_sigma0(uint32_t x) { return rotr32(x,  7) ^ rotr32(x, 18) ^ (x >>  3); }
static uint32_t lil_sigma1(uint32_t x) { return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10); }
static uint32_t ch (uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }

/* === Core: process one 64-byte block ================================ */

static void sha256_process_block(sha256_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4 + 0] << 24)
             | ((uint32_t)block[i * 4 + 1] << 16)
             | ((uint32_t)block[i * 4 + 2] <<  8)
             | ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = lil_sigma1(w[i - 2]) + w[i - 7]
             + lil_sigma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + big_sigma1(e) + ch(e, f, g) + K[i] + w[i];
        uint32_t t2 = big_sigma0(a) + maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

/* === Public API ==================================================== */

void sha256_init(sha256_ctx_t *ctx)
{
    /* FIPS 180-4 §5.3.3 — initial hash value H(0). */
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitcount   = 0;
    ctx->buffer_len = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->bitcount += (uint64_t)len * 8u;

    /* If we have a partial buffer, fill it first. */
    if (ctx->buffer_len > 0) {
        size_t need = 64u - ctx->buffer_len;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->buffer + ctx->buffer_len, p, take);
        ctx->buffer_len += (uint32_t)take;
        p   += take;
        len -= take;
        if (ctx->buffer_len == 64u) {
            sha256_process_block(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }

    /* Process full 64-byte blocks straight from the input. */
    while (len >= 64u) {
        sha256_process_block(ctx, p);
        p   += 64;
        len -= 64;
    }

    /* Tail. */
    if (len > 0) {
        memcpy(ctx->buffer, p, len);
        ctx->buffer_len = (uint32_t)len;
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_LEN])
{
    uint64_t bitcount_be = ctx->bitcount;

    /* Append 0x80, then zero-pad, leaving room for 8-byte length. */
    ctx->buffer[ctx->buffer_len++] = 0x80u;
    if (ctx->buffer_len > 56u) {
        memset(ctx->buffer + ctx->buffer_len, 0, 64u - ctx->buffer_len);
        sha256_process_block(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }
    memset(ctx->buffer + ctx->buffer_len, 0, 56u - ctx->buffer_len);

    /* Append 64-bit length in big-endian. */
    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)((bitcount_be >> (56 - i * 8)) & 0xFFu);
    }
    sha256_process_block(ctx, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)((ctx->state[i] >> 24) & 0xFFu);
        out[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFFu);
        out[i * 4 + 2] = (uint8_t)((ctx->state[i] >>  8) & 0xFFu);
        out[i * 4 + 3] = (uint8_t)( ctx->state[i]        & 0xFFu);
    }
}
