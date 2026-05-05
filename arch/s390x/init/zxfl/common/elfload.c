// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/elfload.c

#include <arch/s390x/init/zxfl/elfload.h>
#include <arch/s390x/init/zxfl/dasd_io.h>
#include <arch/s390x/init/zxfl/dasd_vtoc.h>
#include <arch/s390x/init/zxfl/elf64.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/string.h>
#include <arch/s390x/init/zxfl/zxvl_private.h>
#include <zxfoundation/zconfig.h>

/// @brief Zero memory using MVCL pad-fill mode (srclen=0, pad=0x00).
static void zxfl_bzero(uint64_t addr, uint64_t size) {
    if (size == 0) return;
    register uint64_t r2 __asm__("2") = addr;
    register uint64_t r3 __asm__("3") = size;
    register uint64_t r4 __asm__("4") = addr;  // source addr (unused; srclen=0)
    register uint64_t r5 __asm__("5") = 0;     // srclen=0, pad byte=0x00

    __asm__ volatile (
        "1: mvcl %[r2], %[r4]\n"
        "   jo   1b\n"
        : [r2] "+d" (r2), [r3] "+d" (r3), [r4] "+d" (r4), [r5] "+d" (r5)
        :
        : "cc", "memory"
    );
}

#define ZXVL_RECS_PER_TRACK     12U

/// @brief Translate a byte offset within a multi-extent dataset to CCHHR.
///
///        Iterates over the extent list in order, subtracting the size of
///        each extent until the target offset falls within the current extent.
///        This correctly handles datasets that span multiple DSCB1/DSCB3 extents.
static int offset_to_cchhr(const dasd_dataset_t *ds,
                            uint64_t byte_offset,
                            uint16_t *out_cyl, uint16_t *out_head,
                            uint8_t  *out_rec) {
    uint64_t remaining = byte_offset;

    for (uint32_t e = 0; e < ds->count; e++) {
        const dscb1_extent_t *ext = &ds->extents[e];

        // Tracks in this extent (inclusive).
        uint32_t trk_begin = (uint32_t)ext->begin_cyl * DASD_3390_HEADS_PER_CYL
                             + ext->begin_head;
        uint32_t trk_end   = (uint32_t)ext->end_cyl   * DASD_3390_HEADS_PER_CYL
                             + ext->end_head;
        uint32_t trk_count = trk_end - trk_begin + 1U;
        uint64_t ext_bytes = (uint64_t)trk_count * ZXVL_RECS_PER_TRACK * DASD_BLOCK_SIZE;

        if (remaining < ext_bytes) {
            uint32_t block_in_ext = (uint32_t)(remaining / DASD_BLOCK_SIZE);
            uint32_t trk_in_ext   = block_in_ext / ZXVL_RECS_PER_TRACK;
            uint32_t rec_in_trk   = block_in_ext % ZXVL_RECS_PER_TRACK;

            uint32_t abs_trk = trk_begin + trk_in_ext;
            *out_cyl  = (uint16_t)(abs_trk / DASD_3390_HEADS_PER_CYL);
            *out_head = (uint16_t)(abs_trk % DASD_3390_HEADS_PER_CYL);
            *out_rec  = (uint8_t)(rec_in_trk + 1U);
            return 0;
        }
        remaining -= ext_bytes;
    }
    return -1;   // offset beyond end of dataset
}

static uint8_t io_block[DASD_BLOCK_SIZE] __attribute__((aligned(DASD_BLOCK_SIZE)));

static int load_segment(uint32_t schid,
                        const dasd_dataset_t *ds,
                        const elf64_phdr_t *ph) {
    if (ph->p_memsz == 0) return 0;

    const uint64_t phys_dest = ph->p_paddr - CONFIG_KERNEL_VIRT_OFFSET;

    zxfl_bzero(phys_dest, ph->p_memsz);

    uint64_t file_remaining = ph->p_filesz;
    uint64_t file_offset    = ph->p_offset;
    uint64_t mem_dest       = phys_dest;

    while (file_remaining > 0) {
        uint16_t cyl, head;
        uint8_t  rec;
        if (offset_to_cchhr(ds, file_offset, &cyl, &head, &rec) < 0) {
            print("zxfl: offset beyond dataset extents\n");
            return -1;
        }

        if (dasd_read_record(schid, cyl, head, rec, CCW_CMD_READ_DATA, io_block, DASD_BLOCK_SIZE) < 0) {
            print("zxfl: io error loading segment\n");
            return -1;
        }

        uint32_t block_off = (uint32_t)(file_offset % DASD_BLOCK_SIZE);
        uint32_t avail     = DASD_BLOCK_SIZE - block_off;
        uint32_t copy_len  = (file_remaining < avail) ? (uint32_t)file_remaining : avail;

        memcpy((void *)(uintptr_t)mem_dest, io_block + block_off, copy_len);

        mem_dest       += copy_len;
        file_offset    += copy_len;
        file_remaining -= copy_len;
    }

    return 0;
}

int zxfl_load_elf64(uint32_t schid,
                    const dasd_dataset_t *ds,
                    uint64_t *out_entry,
                    uint64_t *out_load_base,
                    uint64_t *out_load_size,
                    uint64_t hs_nonce) {
    const dscb1_extent_t *ext0 = &ds->extents[0];
    if (dasd_read_record(schid, ext0->begin_cyl, ext0->begin_head, 1, CCW_CMD_READ_DATA, io_block, DASD_BLOCK_SIZE) < 0) {
        print("zxfl: cannot read elf header\n");
        return -1;
    }

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)(uintptr_t)io_block;
    if (!elf64_check_magic(ehdr) || ehdr->e_machine != EM_S390 || ehdr->e_phnum == 0) {
        print("zxfl: bad elf format\n");
        return -1;
    }

    if (ehdr->e_type != ET_EXEC) {
        print("zxvl: nucleus must be ET_EXEC\n");
        return -1;
    }

    if (ehdr->e_entry < CONFIG_KERNEL_VIRT_OFFSET) {
        print("zxvl: nucleus entry point not in higher-half — refusing to load\n");
        return -1;
    }

    *out_entry = ehdr->e_entry;
    const uint16_t phnum = ehdr->e_phnum;
    const uint64_t phoff = ehdr->e_phoff;

    static elf64_phdr_t phdrs[16];
    if (phnum > 16) {
        print("zxfl: too many segments\n");
        return -1;
    }

    uint32_t ph_size = phnum * sizeof(elf64_phdr_t);
    if (phoff + ph_size <= DASD_BLOCK_SIZE) {
        memcpy(phdrs, io_block + phoff, ph_size);
    } else {
        uint16_t pc, ph_head; uint8_t pr;
        if (offset_to_cchhr(ds, phoff, &pc, &ph_head, &pr) < 0) return -1;
        if (dasd_read_record(schid, pc, ph_head, pr, CCW_CMD_READ_DATA, io_block, DASD_BLOCK_SIZE) < 0) return -1;
        uint32_t off = (uint32_t)(phoff % DASD_BLOCK_SIZE);
        memcpy(phdrs, io_block + off, ph_size);
    }

    uint64_t load_min = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t load_max = 0ULL;

    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) continue;

        if (load_segment(schid, ds, &phdrs[i]) < 0) return -1;

        const uint64_t phys = phdrs[i].p_paddr - CONFIG_KERNEL_VIRT_OFFSET;
        if (phys < load_min) load_min = phys;
        uint64_t seg_end = phys + phdrs[i].p_memsz;
        if (seg_end > load_max) load_max = seg_end;
    }

    if (load_min == 0xFFFFFFFFFFFFFFFFULL) return -1;

    print("zxvl: inspecting nucleus\n");
    {
        const uint64_t lock_base = load_min + ZXVL_LOCK_OFFSET;
        const volatile uint32_t *p_hi       = (volatile uint32_t *)(uintptr_t)lock_base;
        const volatile uint32_t *p_sentinel = (volatile uint32_t *)(uintptr_t)(lock_base + 4);
        const volatile uint32_t *p_lo       = (volatile uint32_t *)(uintptr_t)(lock_base + ZXVL_LOCK_GAP);

        if (*p_sentinel != ZXVL_LOCK_SENTINEL) {
            print("zxvl: nucleus lock sentinel missing\n");
            return -1;
        }
        if (((((uint64_t)*p_hi << 32) | (uint64_t)*p_lo) ^ ZXVL_LOCK_MASK) != ZXVL_LOCK_EXPECTED) {
            print("zxvl: nucleus lock failed\n");
            return -1;
        }
    }

    {
        typedef uint64_t (*hs_fn_t)(uint64_t);
        auto stub = (hs_fn_t)(uintptr_t)(load_min + ZXVL_HS_OFFSET);
        if (stub(ZXVL_SEED ^ hs_nonce) != (((hs_nonce << 17) | (hs_nonce >> 47)) + ZXVL_HS_RESPONSE)) {
            print("zxvl: nucleus handshake failed\n");
            return -1;
        }
    }

    *out_load_base = load_min;
    *out_load_size = load_max - load_min;
    return 0;
}
