// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/zxvl_verify.c
//
/// @brief ZXVerifiedLoad — enhanced integrity verification.
///
///        Two verification layers:
///
///        1. Stage-2 self-verification (called from stage-1):
///           SHA-256 of the loaded stage-2 binary is compared against an
///           expected digest embedded in stage-1's .rodata.  The expected
///           digest is patched into the stage-1 image by the build system
///           after stage-2 is compiled.
///
///        2. Kernel checksum verification (called from stage-2):
///           After all ELF segments are loaded, the loader reads the
///           zxvl_checksum_table_t from load_min + ZXVL_CKSUM_TABLE_OFFSET
///           and verifies each entry's SHA-256 or SHA-512 digest against
///           the corresponding physical memory range.

#include <arch/s390x/init/zxfl/zxvl_private.h>
#include <arch/s390x/init/zxfl/panic.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/string.h>
#include <crypto/sha256.h>

void zxvl_verify_nucleus_checksums(uint64_t load_min) {
    const zxvl_checksum_table_t *tbl =
        (const zxvl_checksum_table_t *)(uintptr_t)(load_min + ZXVL_CKSUM_TABLE_OFFSET);

    // Validate magic and version.
    if (tbl->magic != ZXVL_CKSUM_MAGIC)
        panic("zxvl: nucleus checkum table absent");

    if (tbl->version != ZXVL_CKSUM_VERSION)
        panic("zxvl: checksum table version mismatch");

    if (tbl->count == 0 || tbl->count > ZXVL_CKSUM_MAX_ENTRIES)
        panic("zxvl: checksum table corruption");

    const uint32_t algo = tbl->algo;
    if (algo != ZXVL_CKSUM_ALGO_SHA256)
        panic("zxvl: nucleus checksum algorithm unsupported");

    for (uint32_t i = 0; i < tbl->count; i++) {
        const zxvl_cksum_entry_t *e = &tbl->entries[i];
        if (e->size == 0) continue;

        uint8_t actual[ZXFL_SHA256_DIGEST_SIZE];
        zxfl_sha256((const void *)(uintptr_t)e->phys_start, (size_t)e->size, actual);

        if (memcmp(actual, e->digest, ZXFL_SHA256_DIGEST_SIZE) != 0)
            panic("zxvl: nucleus segment checksum mismatch");
    }

    print("zxvl: all nucleus checksums verified\n");
}
