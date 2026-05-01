// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/elfload.c

#include <arch/s390x/init/zxfl/elfload.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/elf64.h>
#include <arch/s390x/init/zxfl/diag.h>

static inline void zxfl_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void zxfl_bzero(uint32_t addr, uint32_t size) {
    if (size == 0) return;
    register uint32_t r0 __asm__("0") = 0;          // Padding byte = 0
    register uint32_t r2 __asm__("2") = addr;
    register uint32_t r3 __asm__("3") = size;
    register uint32_t r4 __asm__("4") = addr;        // Source = destination
    register uint32_t r5 __asm__("5") = 0;           // Source length = 0 → pure fill

    __asm__ volatile (
        "1: mvcl %[r2], %[r4]\n"
        "   jo   1b\n"
        : [r2] "+d" (r2), [r3] "+d" (r3), [r4] "+d" (r4), [r5] "+d" (r5)
        : "d" (r0)
        : "cc", "memory"
    );
}

/// @brief Convert a byte offset within a dataset extent into a (cyl, head, rec)
///        tuple.  The extent begins at (begin_cyl, begin_head).
///
///        Layout assumption: fixed-block records of DASD_BLOCK_SIZE bytes,
///        ZXFL_RECS_PER_TRACK records per track, DASD_3390_HEADS_PER_CYL
///        heads per cylinder.  This matches the sysres.conf FB 4096 4096
///        allocation for sys.zxfoundation.nucleus.
///
///        We deliberately avoid division where possible by using the fact
///        that DASD_BLOCK_SIZE is a power of two.
#define ZXFL_RECS_PER_TRACK     12U

static void offset_to_cchhr(const dscb1_extent_t *ext,
                             uint64_t byte_offset,
                             uint16_t *out_cyl, uint16_t *out_head,
                             uint8_t  *out_rec) {
    uint32_t block_num  = (uint32_t)(byte_offset / DASD_BLOCK_SIZE);
    uint32_t track_num  = block_num / ZXFL_RECS_PER_TRACK;
    uint32_t rec_in_trk = block_num % ZXFL_RECS_PER_TRACK; // 0-based

    uint32_t abs_head = (uint32_t)ext->begin_head + track_num;
    uint32_t abs_cyl  = (uint32_t)ext->begin_cyl  + abs_head / DASD_3390_HEADS_PER_CYL;
    abs_head          = abs_head % DASD_3390_HEADS_PER_CYL;

    *out_cyl  = (uint16_t)abs_cyl;
    *out_head = (uint16_t)abs_head;
    *out_rec  = (uint8_t)(rec_in_trk + 1); // DASD records are 1-based
}

static uint8_t io_block[DASD_BLOCK_SIZE] __attribute__((aligned(DASD_BLOCK_SIZE)));

/// @brief Load one PT_LOAD segment from DASD into physical memory.
///
///        The segment may span multiple DASD tracks and cylinders.
///        We read block-by-block, copying only the bytes that belong to
///        the segment (handling partial first/last blocks).
///
///        p_filesz bytes are read from disk; (p_memsz - p_filesz) bytes
///        are zeroed in memory (BSS).  The load address is p_paddr.
static int load_segment(uint32_t schid,
                        const dscb1_extent_t *ext,
                        const elf64_phdr_t *ph) {
    if (ph->p_filesz == 0 && ph->p_memsz == 0) return 0;

    zxfl_bzero((uint32_t)ph->p_paddr, (uint32_t)ph->p_memsz);

    uint64_t file_remaining = ph->p_filesz;
    uint64_t file_offset    = ph->p_offset;
    uint32_t mem_dest       = (uint32_t)ph->p_paddr;

    while (file_remaining > 0) {
        uint16_t cyl;
        uint16_t head;
        uint8_t  rec;
        offset_to_cchhr(ext, file_offset, &cyl, &head, &rec);

        int rc = dasd_read_record(schid, cyl, head, rec,
                                  CCW_CMD_READ_DATA,
                                  io_block, DASD_BLOCK_SIZE);
        if (rc < 0) {
            print_msg("zxfl: IO error loading segment\n");
            return -1;
        }

        // How many bytes of this block belong to the segment?
        uint32_t block_off = (uint32_t)(file_offset % DASD_BLOCK_SIZE);
        uint32_t avail     = DASD_BLOCK_SIZE - block_off;
        uint32_t copy_len  = (file_remaining < avail)
                             ? (uint32_t)file_remaining
                             : avail;

        zxfl_memcpy((void *)(uintptr_t)mem_dest,
                    io_block + block_off,
                    copy_len);

        mem_dest       += copy_len;
        file_offset    += copy_len;
        file_remaining -= copy_len;
    }

    return 0;
}

int zxfl_load_elf64(uint32_t schid,
                    const dscb1_extent_t *ext,
                    uint64_t *out_entry,
                    uint32_t *out_load_base,
                    uint32_t *out_load_size) {
    uint16_t cyl  = ext->begin_cyl;
    uint16_t head = ext->begin_head;
    uint8_t  rec  = 1;

    int rc = dasd_read_record(schid, cyl, head, rec,
                              CCW_CMD_READ_DATA,
                              io_block, DASD_BLOCK_SIZE);
    if (rc < 0) {
        print_msg("zxfl: cannot read ELF header block\n");
        return -1;
    }

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)(uintptr_t)io_block;
    if (!elf64_check_magic(ehdr)) {
        print_msg("zxfl: bad ELF magic\n");
        return -1;
    }
    if (ehdr->e_machine != EM_S390) {
        print_msg("zxfl: wrong ELF machine type\n");
        return -1;
    }
    if (ehdr->e_phnum == 0) {
        print_msg("zxfl: no program headers\n");
        return -1;
    }

    *out_entry = ehdr->e_entry;

    uint8_t phdr_block[DASD_BLOCK_SIZE] __attribute__((aligned(DASD_BLOCK_SIZE)));
    const elf64_phdr_t *phdrs;

    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(elf64_phdr_t)
            <= DASD_BLOCK_SIZE) {
        phdrs = (const elf64_phdr_t *)(uintptr_t)(io_block + ehdr->e_phoff);
    } else {
        // Rare case: phdr table starts beyond the first block.
        uint16_t pc; uint16_t ph_head; uint8_t pr;
        offset_to_cchhr(ext, ehdr->e_phoff, &pc, &ph_head, &pr);
        rc = dasd_read_record(schid, pc, ph_head, pr,
                              CCW_CMD_READ_DATA,
                              phdr_block, DASD_BLOCK_SIZE);
        if (rc < 0) {
            print_msg("zxfl: cannot read phdr block\n");
            return -1;
        }
        uint32_t off_in_block = (uint32_t)(ehdr->e_phoff % DASD_BLOCK_SIZE);
        phdrs = (const elf64_phdr_t *)(uintptr_t)(phdr_block + off_in_block);
    }

    uint32_t load_min = 0xFFFFFFFFU;
    uint32_t load_max = 0U;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_memsz == 0)      continue;

        rc = load_segment(schid, ext, &phdrs[i]);
        if (rc < 0) return -1;

        uint32_t seg_base = (uint32_t)phdrs[i].p_paddr;
        uint32_t seg_end  = seg_base + (uint32_t)phdrs[i].p_memsz;
        if (seg_base < load_min) load_min = seg_base;
        if (seg_end  > load_max) load_max = seg_end;
    }

    if (load_min == 0xFFFFFFFFU) {
        print_msg("zxfl: no PT_LOAD segments\n");
        return -1;
    }

    *out_load_base = load_min;
    *out_load_size = load_max - load_min;
    return 0;
}
