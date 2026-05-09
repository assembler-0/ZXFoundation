// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/elfload.h

#ifndef ZXFOUNDATION_ZXFL_ELFLOAD_H
#define ZXFOUNDATION_ZXFL_ELFLOAD_H

#include <zxfoundation/types.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/zxfl.h>

/// @brief Load an ELF64 kernel from a multi-extent DASD dataset.
///
///        Reads PT_LOAD segments from the dataset described by @p ds,
///        placing each segment at its p_paddr (physical address from ELF).
///        BSS (p_memsz > p_filesz) is zeroed with MVCL.
///        Multi-extent datasets are fully supported via offset_to_cchhr.
///
///        Special segments are discovered by p_flags fingerprint:
///          ZXVL_PFLAGS_HS   — handshake stub (p_paddr stored as load_base)
///          ZXVL_PFLAGS_LOCK — structural lock (p_paddr → proto->lock_phys)
///          ZXVL_PFLAGS_CKSUM — checksum table (p_paddr → proto->cksum_table_phys)
///
/// @param schid         Subchannel ID of the IPL device
/// @param ds            Full extent list of the nucleus dataset
/// @param proto         Boot protocol; lock_phys and cksum_table_phys are populated
/// @param out_entry     Receives the ELF entry point (e_entry)
/// @param out_load_base Receives the lowest p_paddr across all PT_LOAD segments
/// @param out_load_size Receives the total span (max_end - min_base)
/// @param hs_nonce      Handshake nonce (ZXVL_COMPUTE_TOKEN result)
/// @return 0 on success, -1 on I/O or format error
int zxfl_load_elf64(uint32_t schid,
                    const dasd_dataset_t *ds,
                    zxfl_boot_protocol_t *proto,
                    uint64_t *out_entry,
                    uint64_t *out_load_base,
                    uint64_t *out_load_size,
                    uint64_t hs_nonce);

#endif /* ZXFOUNDATION_ZXFL_ELFLOAD_H */
