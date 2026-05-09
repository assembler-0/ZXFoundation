// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/zxvl_private.h

#ifndef ZXFOUNDATION_ZXVL_PRIVATE_H
#define ZXFOUNDATION_ZXVL_PRIVATE_H

/// @brief Binding token
#define ZXVL_SEED               0xA5F0C3E1B2D49687UL
#define ZXVL_SEED_HI            0xA5F0C3E1U
#define ZXVL_SEED_LO            0xB2D49687U

/// @brief Handshake stub — first PT_LOAD segment (PF_X|PF_R, custom bit 22).
///        The loader calls the stub at the segment's p_paddr (physical).
#define ZXVL_HS_CHALLENGE       0xFEEDFACECAFEBABEUL
#define ZXVL_HS_RESPONSE        0xDEADBEEF0BADF00DUL
#define ZXVL_HS_RESPONSE_HI     0xDEADBEEFU
#define ZXVL_HS_RESPONSE_LO     0x0BADF00DU
/// p_flags value that uniquely identifies the handshake PT_LOAD segment.
#define ZXVL_PFLAGS_HS          0x00400005U   ///< PF_X|PF_R + custom bit 22
/// p_flags value that uniquely identifies the kernel entry PT_LOAD segment.
#define ZXVL_PFLAGS_ENTRY       0x00800005U   ///< PF_X|PF_R + custom bit 23

/// @brief Stack frame canaries
#define ZXVL_FRAME_MAGIC_A      0xC0FFEE00FACADE42UL
#define ZXVL_FRAME_MAGIC_B      0x1337BABE0DDBA115UL
#define ZXVL_FRAME_SIZE         64U

/// @brief Structural lock p_flags fingerprint (PT_LOAD RW, no execute).
///        The loader identifies the lock segment by this unique flag combination.
#define ZXVL_LOCK_MASK          0x3C1E0F8704B2D596UL
#define ZXVL_LOCK_EXPECTED      0xF0A5C3B2E1D49687UL
#define ZXVL_LOCK_EMBED_HI      0xCCBBCC35U
#define ZXVL_LOCK_EMBED_LO      0xE5664311U
#define ZXVL_LOCK_SENTINEL      0x5A58464CU
/// p_flags value that uniquely identifies the lock PT_LOAD segment.
#define ZXVL_PFLAGS_LOCK        0x00100006U   ///< PF_R|PF_W + custom bit 20

/// @brief SHA-256/512 kernel checksum table
///        The loader identifies the checksum segment by p_flags == ZXVL_PFLAGS_CKSUM.
#define ZXVL_CKSUM_MAGIC        0x5A58564CU
#define ZXVL_CKSUM_VERSION      0x00000001U
#define ZXVL_CKSUM_ALGO_SHA256  0x00000001U
#define ZXVL_CKSUM_MAX_ENTRIES  16U
/// p_flags value that uniquely identifies the checksum PT_LOAD segment.
#define ZXVL_PFLAGS_CKSUM       0x00200004U   ///< PF_R + custom bit 21

#ifndef __ASSEMBLER__

#include <arch/s390x/init/zxfl/zxfl.h>
#include <crypto/sha256.h>
#include <zxfoundation/types.h>

#define ZXVL_COMPUTE_TOKEN(stfle0, schid) \
    (ZXVL_SEED ^ (uint64_t)(stfle0) ^ (uint64_t)(schid))

typedef struct __attribute__((packed)) {
    uint64_t phys_start;
    uint64_t size;
    uint8_t  digest[ZXFL_SHA256_DIGEST_SIZE];
} zxvl_cksum_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic, version, algo, count;
    zxvl_cksum_entry_t entries[ZXVL_CKSUM_MAX_ENTRIES];
} zxvl_checksum_table_t;

void zxfl_system_detect(zxfl_boot_protocol_t *proto);

/// @brief Verify all kernel PT_LOAD segments against the embedded checksum table.
///
///        Called by stage-2 after zxfl_load_elf64() succeeds, before the
///        handshake stub is invoked.  Panics on any mismatch.
///
/// @param cksum_table_phys  Physical address of the zxvl_checksum_table_t,
///                          as populated by zxfl_load_elf64() via p_flags scan.
void zxvl_verify_nucleus_checksums(uint64_t cksum_table_phys);

/// @brief Build timestamp
#define _ZX_CH2(s, i)   ((uint32_t)((s)[i] - '0') * 10U + (uint32_t)((s)[i+1] - '0'))
#define ZXVL_BUILD_TS   ((_ZX_CH2(__TIME__, 0) << 24) | \
                         (_ZX_CH2(__TIME__, 3) << 16) | \
                         (_ZX_CH2(__TIME__, 6) <<  8) | \
                          0x5AU)

#endif /* !__ASSEMBLER__ */

#endif /* ZXFOUNDATION_ZXVL_PRIVATE_H */
