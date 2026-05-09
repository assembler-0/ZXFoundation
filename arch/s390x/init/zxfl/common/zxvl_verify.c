// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/zxvl_verify.c
//
/// @brief ZXVerifiedLoad — kernel checksum verification.
///
///        The checksum table physical address is discovered dynamically by
///        elfload.c scanning PT_LOAD p_flags for ZXVL_PFLAGS_CKSUM.
///        No hardcoded offsets are used here.

#include <arch/s390x/init/zxfl/zxvl_private.h>
#include <arch/s390x/init/zxfl/panic.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/string.h>
#include <crypto/sha256.h>

void zxvl_verify_nucleus_checksums(uint64_t cksum_table_phys) {
    const zxvl_checksum_table_t *tbl =
        (const zxvl_checksum_table_t *)(uintptr_t)cksum_table_phys;

    if (tbl->magic != ZXVL_CKSUM_MAGIC)
        panic("zxvl: nucleus checksum table absent");

    if (tbl->version != ZXVL_CKSUM_VERSION)
        panic("zxvl: checksum table version mismatch");

    if (tbl->count == 0 || tbl->count > ZXVL_CKSUM_MAX_ENTRIES)
        panic("zxvl: checksum table corruption");

    if (tbl->algo != ZXVL_CKSUM_ALGO_SHA256)
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
