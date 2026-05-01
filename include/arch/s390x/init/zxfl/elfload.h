// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/elfload.h
//
/// @brief ELF64 kernel loader API for the ZXFL bootloader.

#ifndef ZXFOUNDATION_ZXFL_ELFLOAD_H
#define ZXFOUNDATION_ZXFL_ELFLOAD_H

#include <zxfoundation/types.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>

/// @brief Load an ELF64 kernel from DASD into physical memory.
///
///        Reads PT_LOAD segments from the dataset extent described by @p ext,
///        placing each segment at its p_paddr (physical address from the ELF).
///        BSS (p_memsz > p_filesz) is zeroed with MVCL.
///        No address is hardcoded — the load base is derived entirely from
///        the ELF program headers.
///
/// @param schid         Subchannel ID of the IPL device
/// @param ext           First extent of the sys.zxfoundation.nucleus dataset
/// @param out_entry     Receives the ELF entry point (e_entry)
/// @param out_load_base Receives the lowest p_paddr across all PT_LOAD segments
/// @param out_load_size Receives the total span (max_end - min_base)
/// @return 0 on success, -1 on I/O or format error
int zxfl_load_elf64(uint32_t schid,
                    const dscb1_extent_t *ext,
                    uint64_t *out_entry,
                    uint32_t *out_load_base,
                    uint32_t *out_load_size);

#endif /* ZXFOUNDATION_ZXFL_ELFLOAD_H */
