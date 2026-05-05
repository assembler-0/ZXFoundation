// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/zxvl_private.h

#ifndef ZXFOUNDATION_ZXVL_PRIVATE_H
#define ZXFOUNDATION_ZXVL_PRIVATE_H

#include <stdint.h>
#include "zxfl.h"

void zxfl_system_detect(zxfl_boot_protocol_t *proto);

/// @brief Binding token
#define ZXVL_SEED               UINT64_C(0xA5F0C3E1B2D49687)

#define ZXVL_COMPUTE_TOKEN(stfle0, schid) \
    (ZXVL_SEED ^ (uint64_t)(stfle0) ^ (uint64_t)(schid))

/// @brief Handshake stub (at load_min + ZXVL_HS_OFFSET)
#define ZXVL_HS_OFFSET          0x0UL
#define ZXVL_HS_CHALLENGE       UINT64_C(0xFEEDFACECAFEBABE)
#define ZXVL_HS_RESPONSE        UINT64_C(0xDEADBEEF0BADF00D)

/// @brief Stack frame canaries
#define ZXVL_FRAME_MAGIC_A      UINT64_C(0xC0FFEE00FACADE42)
#define ZXVL_FRAME_MAGIC_B      UINT64_C(0x1337BABE0DDBA115)
#define ZXVL_FRAME_SIZE         64U

/// @brief Structural lock (at load_min + ZXVL_LOCK_OFFSET)
#define ZXVL_LOCK_MASK          UINT64_C(0x3C1E0F8704B2D596)
#define ZXVL_LOCK_EXPECTED      UINT64_C(0xF0A5C3B2E1D49687)
#define ZXVL_LOCK_EMBED_HI      0xCCBBCC35U
#define ZXVL_LOCK_EMBED_LO      0xE5664311U
#define ZXVL_LOCK_OFFSET        0x70000U
#define ZXVL_LOCK_GAP           0x1000U
#define ZXVL_LOCK_SENTINEL      0x5A58464CU

/// @brief SHA-256/512 kernel checksum table

#define ZXVL_CKSUM_MAGIC        0x5A58564CU
#define ZXVL_CKSUM_VERSION      0x00000001U
#define ZXVL_CKSUM_ALGO_SHA256  0x00000001U
#define ZXVL_CKSUM_MAX_ENTRIES  16U
#define ZXVL_CKSUM_TABLE_OFFSET 0x80000U

typedef struct __attribute__((packed)) {
    uint64_t phys_start;
    uint64_t size;
    uint8_t  digest[ZXFL_SHA256_DIGEST_SIZE];
} zxvl_cksum_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic, version, algo, count;
    zxvl_cksum_entry_t entries[ZXVL_CKSUM_MAX_ENTRIES];
} zxvl_checksum_table_t;


/// @brief Verify all kernel PT_LOAD segments against the embedded checksum table.
///
///        Called by stage-2 after zxfl_load_elf64() succeeds, before the
///        handshake stub is invoked.  Panics on any mismatch.
///
/// @param load_min  Physical base of the loaded kernel image
void zxvl_verify_nucleus_checksums(uint64_t load_min);

/// @brief Build timestamp
#define _ZX_CH2(s, i)   ((uint32_t)((s)[i] - '0') * 10U + (uint32_t)((s)[i+1] - '0'))
#define ZXVL_BUILD_TS   ((_ZX_CH2(__TIME__, 0) << 24) | \
                         (_ZX_CH2(__TIME__, 3) << 16) | \
                         (_ZX_CH2(__TIME__, 6) <<  8) | \
                          0x5AU)

#endif /* ZXFOUNDATION_ZXVL_PRIVATE_H */
