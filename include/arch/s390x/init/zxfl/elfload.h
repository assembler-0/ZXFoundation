// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/elfload.h

#ifndef ZXFOUNDATION_ZXFL_ELFLOAD_H
#define ZXFOUNDATION_ZXFL_ELFLOAD_H

#include <zxfoundation/types.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>

/// @brief Load an ELF64 kernel from a multi-extent DASD dataset.
///
///        Reads PT_LOAD segments from the dataset described by @p ds,
///        placing each segment at its p_paddr (physical address from ELF).
///        BSS (p_memsz > p_filesz) is zeroed with MVCL.
///        Multi-extent datasets are fully supported via offset_to_cchhr.
///
/// @param schid         Subchannel ID of the IPL device
/// @param ds            Full extent list of the nucleus dataset
/// @param out_entry     Receives the ELF entry point (e_entry)
/// @param out_load_base Receives the lowest p_paddr across all PT_LOAD segments
/// @param out_load_size Receives the total span (max_end - min_base)
/// @param hs_nonce      Handshake nonce (ZXVL_COMPUTE_TOKEN result)
/// @return 0 on success, -1 on I/O or format error
int zxfl_load_elf64(uint32_t schid,
                    const dasd_dataset_t *ds,
                    uint64_t *out_entry,
                    uint64_t *out_load_base,
                    uint64_t *out_load_size,
                    uint64_t hs_nonce);

#endif /* ZXFOUNDATION_ZXFL_ELFLOAD_H */
