// SPDX-License-Identifier: Apache-2.0
// crypto/sha256.c
//
/// @brief Freestanding SHA-256 (FIPS 180-4).

#include <crypto/sha256.h>

static const uint32_t K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return __builtin_rotateright32(x, n);
}

/// @brief SHA-256 block compression (FIPS 180-4 §6.2.2).
static void sha256_block(zxfl_sha256_ctx_t *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (uint32_t i = 0; i < 16U; i++) {
        w[i] = ((uint32_t)block[i * 4U + 0U] << 24U) |
               ((uint32_t)block[i * 4U + 1U] << 16U) |
               ((uint32_t)block[i * 4U + 2U] <<  8U) |
               ((uint32_t)block[i * 4U + 3U]);
    }
    for (uint32_t i = 16U; i < 64U; i++) {
        uint32_t s0 = rotr32(w[i-15U],  7U) ^ rotr32(w[i-15U], 18U) ^ (w[i-15U] >>  3U);
        uint32_t s1 = rotr32(w[i- 2U], 17U) ^ rotr32(w[i- 2U], 19U) ^ (w[i- 2U] >> 10U);
        w[i] = w[i-16U] + s0 + w[i-7U] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (uint32_t i = 0; i < 64U; i++) {
        uint32_t S1  = rotr32(e,  6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
        uint32_t ch  = (e & f) ^ (~e & g);
        uint32_t t1  = h + S1 + ch + K[i] + w[i];
        uint32_t S0  = rotr32(a,  2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2  = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void zxfl_sha256_init(zxfl_sha256_ctx_t *ctx) {
    // Initial hash values: first 32 bits of fractional parts of square roots
    // of the first 8 primes (FIPS 180-4 §5.3.3).
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->bit_count = 0;
    ctx->buf_len   = 0;
}

void zxfl_sha256_update(zxfl_sha256_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->bit_count += (uint64_t)len * 8U;

    while (len > 0) {
        uint32_t space = 64U - ctx->buf_len;
        uint32_t take  = (len < space) ? (uint32_t)len : space;
        for (uint32_t i = 0; i < take; i++)
            ctx->buf[ctx->buf_len + i] = p[i];
        ctx->buf_len += take;
        p   += take;
        len -= take;
        if (ctx->buf_len == 64U) {
            sha256_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

void zxfl_sha256_final(zxfl_sha256_ctx_t *ctx, uint8_t digest[ZXFL_SHA256_DIGEST_SIZE]) {
    // Append the mandatory 0x80 padding byte.
    ctx->buf[ctx->buf_len++] = 0x80U;

    // If there is not enough room for the 8-byte length field, flush and
    // start a new padding block.
    if (ctx->buf_len > 56U) {
        while (ctx->buf_len < 64U) ctx->buf[ctx->buf_len++] = 0;
        sha256_block(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56U) ctx->buf[ctx->buf_len++] = 0;

    // Append message length in bits as a 64-bit big-endian integer.
    const uint64_t bits = ctx->bit_count;
    ctx->buf[56] = (uint8_t)(bits >> 56U);
    ctx->buf[57] = (uint8_t)(bits >> 48U);
    ctx->buf[58] = (uint8_t)(bits >> 40U);
    ctx->buf[59] = (uint8_t)(bits >> 32U);
    ctx->buf[60] = (uint8_t)(bits >> 24U);
    ctx->buf[61] = (uint8_t)(bits >> 16U);
    ctx->buf[62] = (uint8_t)(bits >>  8U);
    ctx->buf[63] = (uint8_t)(bits);
    sha256_block(ctx, ctx->buf);

    // Serialize state as big-endian bytes.
    for (uint32_t i = 0; i < 8U; i++) {
        digest[i * 4U + 0U] = (uint8_t)(ctx->state[i] >> 24U);
        digest[i * 4U + 1U] = (uint8_t)(ctx->state[i] >> 16U);
        digest[i * 4U + 2U] = (uint8_t)(ctx->state[i] >>  8U);
        digest[i * 4U + 3U] = (uint8_t)(ctx->state[i]);
    }
}
