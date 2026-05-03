// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/elf64.h
//
/// @brief Minimal ELF64 type definitions for the ZXFL bootloader.

#ifndef ZXFOUNDATION_ZXFL_ELF64_H
#define ZXFOUNDATION_ZXFL_ELF64_H

#include <zxfoundation/types.h>

#define ELF_MAGIC0      0x7FU
#define ELF_MAGIC1      'E'
#define ELF_MAGIC2      'L'
#define ELF_MAGIC3      'F'

#define ET_EXEC         2U      ///< Executable file
#define EM_S390         22U     ///< IBM S/390 (covers both ESA/390 and z/Arch)
#define PT_LOAD         1U      ///< Loadable segment
#define ELFCLASS64      2U      ///< 64-bit objects

typedef struct {
    uint8_t  e_ident[16];   ///< Magic, class, data, version, OS/ABI, padding
    uint16_t e_type;        ///< Object file type (ET_EXEC)
    uint16_t e_machine;     ///< Architecture (EM_S390)
    uint32_t e_version;     ///< Object file version (1)
    uint64_t e_entry;       ///< Virtual entry point address
    uint64_t e_phoff;       ///< Program header table file offset
    uint64_t e_shoff;       ///< Section header table file offset (unused)
    uint32_t e_flags;       ///< Processor-specific flags
    uint16_t e_ehsize;      ///< ELF header size in bytes (64)
    uint16_t e_phentsize;   ///< Program header entry size (56)
    uint16_t e_phnum;       ///< Number of program header entries
    uint16_t e_shentsize;   ///< Section header entry size
    uint16_t e_shnum;       ///< Number of section header entries
    uint16_t e_shstrndx;    ///< Section name string table index
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;        ///< Segment type (PT_LOAD = 1)
    uint32_t p_flags;       ///< Segment flags (PF_X=1, PF_W=2, PF_R=4)
    uint64_t p_offset;      ///< Offset of segment data in file
    uint64_t p_vaddr;       ///< Virtual address in memory
    uint64_t p_paddr;       ///< Physical address (same as vaddr pre-MMU)
    uint64_t p_filesz;      ///< Size of segment in file
    uint64_t p_memsz;       ///< Size of segment in memory (>= p_filesz for BSS)
    uint64_t p_align;       ///< Alignment (power of 2; 0 or 1 = no alignment)
} elf64_phdr_t;

/// @brief Return non-zero if the ELF magic bytes are valid.
static inline int elf64_check_magic(const elf64_ehdr_t *h) {
    return (h->e_ident[0] == ELF_MAGIC0 &&
            h->e_ident[1] == ELF_MAGIC1 &&
            h->e_ident[2] == ELF_MAGIC2 &&
            h->e_ident[3] == ELF_MAGIC3 &&
            h->e_ident[4] == ELFCLASS64);
}

#endif /* ZXFOUNDATION_ZXFL_ELF64_H */
