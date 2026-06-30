// SPDX-License-Identifier: Apache-2.0
// crypto/sha256.c
//
/// @brief Freestanding SHA-256 (FIPS 180-4).

#include <arch/s390x/init/zxfl/sha256.h>

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
    return (x >> n) | (x << (32U - n));
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

/// @brief Software one-shot SHA-256 (always available).
static void zxfl_sha256_sw(const void *data, size_t len,
                           uint8_t digest[ZXFL_SHA256_DIGEST_SIZE]) {
    zxfl_sha256_ctx_t ctx;
    zxfl_sha256_init(&ctx);
    zxfl_sha256_update(&ctx, data, len);
    zxfl_sha256_final(&ctx, digest);
}

#if defined(__s390x__) || defined(__s390__) || defined(__zarch__)

#include <arch/s390x/init/zxfl/stfle.h>

/// KIMD function code for SHA-256 (PoP SA22-7832, Message-Security-Assist).
#define ZXFL_CPACF_KIMD_SHA_256   2UL

/// @brief Run KIMD (COMPUTE INTERMEDIATE MESSAGE DIGEST) for SHA-256.
/// @param param    32-byte chaining value (also receives the result).
/// @param src      Message segment, length a multiple of 64 bytes.
/// @param src_len  Segment length in bytes (multiple of 64).
/// @warning The Message-Security-Assist facility MUST be installed before this
///          is executed, otherwise an operation exception results.
static inline void zxfl_cpacf_kimd_sha256(void *param, const uint8_t *src,
                                          unsigned long src_len) {
    register unsigned long a __asm__("2") = (unsigned long)src;  // even reg
    register unsigned long l __asm__("3") = src_len;             // odd reg
    __asm__ volatile(
        "   lgr   0,%[fc]\n"                    // GR0 = function code
        "   lgr   1,%[pb]\n"                    // GR1 = parameter-block address
        "0: .insn rre,0xb93e0000,0,%[a]\n"      // KIMD: r1 ignored, r2 = pair
        "   brc   1,0b\n"                        // retry on partial completion
        : [a] "+d"(a), [l] "+d"(l)
        : [fc] "d"(ZXFL_CPACF_KIMD_SHA_256), [pb] "d"((unsigned long)param)
        : "cc", "memory", "0", "1");
}

/// @brief Hardware one-shot SHA-256 via CPACF KIMD.
static void zxfl_sha256_hw(const void *data, size_t len,
                           uint8_t digest[ZXFL_SHA256_DIGEST_SIZE]) {
    uint32_t state[8] __attribute__((aligned(8))) = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    const uint8_t *p = (const uint8_t *)data;

    const size_t full = len & ~(size_t)63;   // whole 64-byte blocks
    if (full)
        zxfl_cpacf_kimd_sha256(state, p, (unsigned long)full);

    uint8_t tail[128] __attribute__((aligned(8)));
    const size_t rem = len - full;
    for (size_t i = 0; i < rem; i++)
        tail[i] = p[full + i];
    tail[rem] = 0x80U;

    // One padding block unless the 0x80 byte leaves no room for the 8-byte
    // length field (i.e. rem >= 56), in which case a second block is needed.
    const size_t block_bytes = (rem <= 55U) ? 64U : 128U;
    for (size_t i = rem + 1U; i < block_bytes - 8U; i++)
        tail[i] = 0U;

    const uint64_t bit_len = (uint64_t)len * 8U;
    tail[block_bytes - 8U] = (uint8_t)(bit_len >> 56U);
    tail[block_bytes - 7U] = (uint8_t)(bit_len >> 48U);
    tail[block_bytes - 6U] = (uint8_t)(bit_len >> 40U);
    tail[block_bytes - 5U] = (uint8_t)(bit_len >> 32U);
    tail[block_bytes - 4U] = (uint8_t)(bit_len >> 24U);
    tail[block_bytes - 3U] = (uint8_t)(bit_len >> 16U);
    tail[block_bytes - 2U] = (uint8_t)(bit_len >>  8U);
    tail[block_bytes - 1U] = (uint8_t)(bit_len);

    zxfl_cpacf_kimd_sha256(state, tail, (unsigned long)block_bytes);

    for (uint32_t i = 0; i < 8U; i++) {
        digest[i * 4U + 0U] = (uint8_t)(state[i] >> 24U);
        digest[i * 4U + 1U] = (uint8_t)(state[i] >> 16U);
        digest[i * 4U + 2U] = (uint8_t)(state[i] >>  8U);
        digest[i * 4U + 3U] = (uint8_t)(state[i]);
    }
}

/// Tri-state cache: 0 = undecided, 1 = hardware verified, -1 = software only.
static int g_sha256_hw = 0;

/// @brief Decide whether the CPACF SHA-256 path is safe to use.
static bool zxfl_sha256_hw_selftest(void) {
    uint64_t fac[1] = { 0 };
    if (stfle_detect(fac, 1U) == 0)
        return false;
    if (!stfle_has_facility(fac, STFLE_BIT_MSA))
        return false;

    static const uint8_t kat_in[3] = { 'a', 'b', 'c' };
    static const uint8_t kat_expect[ZXFL_SHA256_DIGEST_SIZE] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    uint8_t out[ZXFL_SHA256_DIGEST_SIZE];
    zxfl_sha256_hw(kat_in, sizeof(kat_in), out);
    for (uint32_t i = 0; i < ZXFL_SHA256_DIGEST_SIZE; i++)
        if (out[i] != kat_expect[i])
            return false;
    return true;
}

void zxfl_sha256(const void *data, size_t len,
                 uint8_t digest[ZXFL_SHA256_DIGEST_SIZE]) {
    if (g_sha256_hw == 0)
        g_sha256_hw = zxfl_sha256_hw_selftest() ? 1 : -1;
    if (g_sha256_hw == 1) {
        zxfl_sha256_hw(data, len, digest);
        return;
    }
    zxfl_sha256_sw(data, len, digest);
}

#endif