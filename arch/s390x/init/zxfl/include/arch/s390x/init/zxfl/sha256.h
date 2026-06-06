// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/sha256.h
//
/// @brief Freestanding SHA-256 for the ZXFL bootloader.
///        No libc, no dynamic allocation.

#ifndef ZXFOUNDATION_ZXFL_SHA256_H
#define ZXFOUNDATION_ZXFL_SHA256_H

#include <zxfoundation/types.h>

#define ZXFL_SHA256_DIGEST_SIZE  32U

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t  buf[64];
    uint32_t buf_len;
} zxfl_sha256_ctx_t;

/// @brief Initialize a SHA-256 context.
void zxfl_sha256_init(zxfl_sha256_ctx_t *ctx);

/// @brief Feed bytes into the hash.
void zxfl_sha256_update(zxfl_sha256_ctx_t *ctx, const void *data, size_t len);

/// @brief Finalize and write the 32-byte digest.
void zxfl_sha256_final(zxfl_sha256_ctx_t *ctx, uint8_t digest[ZXFL_SHA256_DIGEST_SIZE]);

/// @brief One-shot SHA-256: hash data and write digest.
/// @param data    Input bytes
/// @param len     Number of bytes
/// @param digest  Output buffer (must be ZXFL_SHA256_DIGEST_SIZE bytes)
static inline void zxfl_sha256(const void *data, size_t len,
                                uint8_t digest[ZXFL_SHA256_DIGEST_SIZE]) {
    zxfl_sha256_ctx_t ctx;
    zxfl_sha256_init(&ctx);
    zxfl_sha256_update(&ctx, data, len);
    zxfl_sha256_final(&ctx, digest);
}

#endif /* ZXFOUNDATION_ZXFL_SHA256_H */
