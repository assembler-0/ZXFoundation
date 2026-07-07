// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/mmu.c
//
/// @brief z/Architecture 5-level DAT setup for the ZXFL bootloader.
///
///        Builds the full z/Architecture paging hierarchy with a Region-First
///        table root (DT=11) and identity-maps + HHDM-maps all detected
///        physical memory, then enables DAT and LPSWE into the kernel.
///
///        5-level paging hierarchy (PoP SA22-7832-13, Chapter 3):
///          ASCE (DT=11) → R1 Table → R2 Table → R3 Table → Seg Table → Page Table
///                          2048        2048       2048       2048        256
///                          TT=11       TT=10      TT=01      TT=00
///
///        Virtual address bit decomposition for DT=11 (R1 root):
///          Bits 0-10:  RFX — Region-First-Table index   (R1, 2048 entries)
///          Bits 11-21: RSX — Region-Second-Table index  (R2, 2048 entries)
///          Bits 22-32: RTX — Region-Third-Table index   (R3, 2048 entries)
///          Bits 33-43: SX  — Segment-Table index        (Seg, 2048 entries)
///          Bits 44-51: PX  — Page-Table index           (PT, 256 entries)
///          Bits 52-63: BX  — Byte index within page     (4096 bytes)
///
///        Memory coverage per hierarchy level:
///          One R1 entry  → 8 PB   (2048 R2 entries × 4 TB)
///          One R2 entry  → 4 TB   (2048 R3 entries × 2 GB)
///          One R3 entry  → 2 GB   (2048 segment entries × 1 MB)
///          One STE       → 1 MB   (256 page entries × 4 KB)
///          One PTE       → 4 KB
///
///        HHDM mapping for CONFIG_KERNEL_VIRT_OFFSET = 0xFFFF800000000000:
///          RFX = 2047 (topmost R1 entry — clean kernel/user separation)
///          RSX = 2016
///          RTX = 0
///          Both identity (RFX=0) and HHDM (RFX=2047) share the same R2
///          table because their RSX indices do not overlap (0 vs 2016).

#include <arch/s390x/init/zxfl/processor.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/stfle.h>
#include <arch/s390x/init/zxfl/psw.h>
#include <arch/s390x/init/zxfl/string.h>
#include <zxfoundation/memory/hhdm.h>


/// ASCE Designation-Type field (bits 60-61).
/// DT=11 means the root table is a Region-First table (5-level paging).
#define Z_ASCE_DT_R1        0x0CULL

/// ASCE Table-Length field (bits 62-63).
/// TL=11 means the table has 2048 entries.
#define Z_ASCE_TL_2048      0x03ULL

/// Region/Segment Table Entry: Invalid bit (bit 58).
#define Z_I                  0x20ULL

/// Region Table Entry: Table-Type field (bits 60-61).
/// TT identifies which level THIS entry belongs to.
#define Z_TT_R1              0x0CULL   // TT=11: entry belongs to R1 table
#define Z_TT_R2              0x08ULL   // TT=10: entry belongs to R2 table
#define Z_TT_R3              0x04ULL   // TT=01: entry belongs to R3 table
#define Z_TT_SEG             0x00ULL   // TT=00: entry belongs to Segment table

/// Region Table Entry: Table-Length field (bits 62-63).
#define Z_TL_2048            0x03ULL

/// Segment Table Entry: Format-Control bit (bit 53 IBM = LSB bit 10).
/// With EDAT-1, the STE directly maps a 1 MB large page.
#define Z_STE_FC             0x400ULL

/// Page Table Entry: Invalid bit (bit 53).
#define Z_PTE_I              0x400ULL

/// STFLE facility bit for EDAT-1 (enhanced DAT 1).
/// Defined in stfle.h as STFLE_BIT_EDAT1 = 8.

/// Bytes per 1 MB segment.
#define SEG_SIZE             (1024ULL * 1024ULL)

/// Entries per segment table.
#define SEG_TABLE_ENTRIES    2048U

/// Bytes covered by one segment table (2 GB).
#define SEG_TABLE_COVERAGE   ((uint64_t)SEG_TABLE_ENTRIES * SEG_SIZE)

/// Entries per page table.
#define PAGE_TABLE_ENTRIES   256U

/// Entries per Region-3 table.
#define R3_TABLE_ENTRIES     2048U

/// Maximum physical memory the HHDM will map.
///
/// This covers the ENTIRE installed RAM — phys_to_virt() must work for
/// every valid physical address.  A single z16 drawer holds up to 2 TB;
/// a 4-drawer system can exceed 8 TB.  We cap at 16 TB which covers
/// any practical z/Architecture LPAR with comfortable headroom.
///
/// Page-table overhead with EDAT-1 (1 MB large pages):
///   512 GB  →  ~4 MB       2 TB  → ~16 MB
///     4 TB  → ~32 MB      16 TB  → ~128 MB
///
/// Projects that depend on the loader's HHDM for the kernel's entire
/// lifetime should set this high enough for their target hardware.
#define MAX_PHYS_MAP         (16ULL * 1024 * 1024 * 1024 * 1024)

static uint64_t r1_table[2048] __attribute__((aligned(16384)));
static uint64_t r2_table_ident[2048] __attribute__((aligned(16384)));
static uint64_t r2_table_hhdm[2048] __attribute__((aligned(16384)));

/// @brief Linear bump allocator for page-table memory.
static uint64_t pool_next = 0;

/// @brief Allocate 'size' bytes from the pool, aligned to 'align'.
static uint64_t pool_alloc(uint64_t size, uint64_t align) {
    pool_next = (pool_next + align - 1) & ~(align - 1);
    uint64_t addr = pool_next;
    pool_next += size;
    return addr;
}

/// @brief Allocate and initialize a Region-3 table (16 KB, 16KB-aligned).
static uint64_t *alloc_r3_table(void) {
    uint64_t addr = pool_alloc(2048 * 8, 16384);
    uint64_t *tbl = (uint64_t *)(uintptr_t)addr;
    for (uint32_t i = 0; i < 2048; i++) {
        tbl[i] = Z_I | Z_TL_2048 | Z_TT_R3;
    }
    return tbl;
}

/// @brief Allocate and initialize a segment table (16 KB, 16KB-aligned).
static uint64_t *alloc_seg_table(void) {
    uint64_t addr = pool_alloc(SEG_TABLE_ENTRIES * 8, 16384);
    uint64_t *tbl = (uint64_t *)(uintptr_t)addr;
    for (uint32_t i = 0; i < SEG_TABLE_ENTRIES; i++) {
        tbl[i] = Z_I | Z_TT_SEG;
    }
    return tbl;
}

/// @brief Allocate and initialize a page table (2 KB, 2KB-aligned).
static uint64_t *alloc_page_table(void) {
    uint64_t addr = pool_alloc(PAGE_TABLE_ENTRIES * 8, 2048);
    uint64_t *tbl = (uint64_t *)(uintptr_t)addr;
    for (uint32_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        tbl[i] = Z_PTE_I;
    }
    return tbl;
}

/// @brief Build 5-level page tables, enable DAT, and LPSWE into the kernel.
/// @param entry      Kernel entry point (HHDM virtual address from ELF e_entry).
/// @param boot_proto Physical address of the zxfl_boot_protocol_t struct.
[[noreturn]] void zxfl_mmu_setup_and_jump(uint64_t entry, uint64_t boot_proto) {
    zxfl_boot_protocol_t *proto = (zxfl_boot_protocol_t *)boot_proto;
    const uint64_t virt_base = CONFIG_KERNEL_VIRT_OFFSET;

    const bool has_edat1 = stfle_has_facility(proto->stfle_fac, STFLE_BIT_EDAT1);

    // We map ALL usable physical memory up to the highest probed byte.
    uint64_t map_bytes = proto->mem_total_bytes;
    // Find the actual highest physical address in the map
    const zxfl_mem_region_t *mmap = (const zxfl_mem_region_t *)(uintptr_t)proto->mem_map_addr;
    for (uint32_t i = 0; i < proto->mem_map_count; i++) {
        uint64_t end = mmap[i].base + mmap[i].length;
        if (end > map_bytes) map_bytes = end;
    }

    if (map_bytes < 0x200000ULL) map_bytes = 0x200000ULL;
    map_bytes = (map_bytes + SEG_TABLE_COVERAGE - 1) & ~(SEG_TABLE_COVERAGE - 1);

    // Hard limit — see MAX_PHYS_MAP definition for rationale.
    if (map_bytes > MAX_PHYS_MAP) map_bytes = MAX_PHYS_MAP;

    for (uint32_t i = 0; i < 2048; i++) {
        r1_table[i] = Z_I | Z_TL_2048 | Z_TT_R1;
        r2_table_ident[i] = Z_I | Z_TL_2048 | Z_TT_R2;
        r2_table_hhdm[i] = Z_I | Z_TL_2048 | Z_TT_R2;
    }

    uint64_t pool_base = (proto->kernel_phys_end + 0xFFFFFULL) & ~0xFFFFFULL;
    if (pool_base < 0x2000000ULL) pool_base = 0x2000000ULL;
    pool_next = pool_base;

    // Compute R1/R2 index offsets for identity and HHDM mappings.
    const uint32_t rfx_identity = 0;
    const uint32_t rfx_hhdm     = (uint32_t)((virt_base >> 53) & 0x7FFULL);
    const uint32_t rsx_hhdm     = (uint32_t)((virt_base >> 42) & 0x7FFULL);

    // Build the Region-3 and Segment tables.
    // Each R3 table covers 4 TB (2048 entries × 2 GB).
    // Each R2 entry points to one R3 table.
    // For >4 TB, we need multiple R3 tables (one per 4 TB).
    const uint64_t r3_coverage = (uint64_t)R3_TABLE_ENTRIES * SEG_TABLE_COVERAGE;
    const uint32_t num_r2_entries = (uint32_t)((map_bytes + r3_coverage - 1) / r3_coverage);

    // --- Relocate BSP Lowcore ---
    // Allocate 8KB for the new prefix area (lowcore), 8KB aligned.
    uint64_t bsp_lowcore_phys = pool_alloc(8192, 8192);
    memset((void*)(uintptr_t)bsp_lowcore_phys, 0, 8192);
    
    // Pass the relocated lowcore address to the kernel.
    proto->lowcore_phys = bsp_lowcore_phys;
    proto->flags |= ZXFL_FLAG_LOWCORE;

    for (uint32_t r2e = 0; r2e < num_r2_entries; r2e++) {
        uint64_t *r3_tab = alloc_r3_table();

        // How many R3 entries are needed in this slice?
        uint64_t slice_start = (uint64_t)r2e * r3_coverage;
        uint64_t slice_end   = slice_start + r3_coverage;
        if (slice_end > map_bytes) slice_end = map_bytes;
        const uint32_t num_r3e = (uint32_t)((slice_end - slice_start + SEG_TABLE_COVERAGE - 1) / SEG_TABLE_COVERAGE);

        for (uint32_t r3e = 0; r3e < num_r3e; r3e++) {
            uint64_t *seg_table = alloc_seg_table();

            for (uint32_t sx = 0; sx < SEG_TABLE_ENTRIES; sx++) {
                uint64_t phys = slice_start + (uint64_t)r3e * SEG_TABLE_COVERAGE + (uint64_t)sx * SEG_SIZE;
                if (phys >= map_bytes) {
                    seg_table[sx] = Z_I | Z_TT_SEG;
                    continue;
                }

                if (has_edat1) {
                    // EDAT-1: Directly map 1 MB large page in the segment table.
                    seg_table[sx] = phys | Z_STE_FC | Z_TT_SEG;
                } else {
                    // Legacy: Map 256 x 4 KB pages via a page table.
                    uint64_t *pt = alloc_page_table();
                    for (uint32_t p = 0; p < PAGE_TABLE_ENTRIES; p++) {
                        pt[p] = phys + ((uint64_t)p * 4096ULL);
                    }
                    seg_table[sx] = (uint64_t)(uintptr_t)pt | Z_TT_SEG;
                }
            }
            r3_tab[r3e] = (uint64_t)(uintptr_t)seg_table | Z_TL_2048 | Z_TT_R3;
        }

        // Wire this R3 table into BOTH R2 tables (identity + HHDM).
        r2_table_ident[r2e] = (uint64_t)(uintptr_t)r3_tab | Z_TL_2048 | Z_TT_R2;
        r2_table_hhdm[rsx_hhdm + r2e] = (uint64_t)(uintptr_t)r3_tab | Z_TL_2048 | Z_TT_R2;
    }

    r1_table[rfx_identity] = (uint64_t)(uintptr_t)r2_table_ident | Z_TL_2048 | Z_TT_R1;
    r1_table[rfx_hhdm]     = (uint64_t)(uintptr_t)r2_table_hhdm | Z_TL_2048 | Z_TT_R1;

    // Update protocol addresses to be HHDM-virtual.
    proto->kernel_stack_top = hhdm_phys_to_virt(proto->kernel_stack_top);
    proto->mem_map_addr     = hhdm_phys_to_virt(proto->mem_map_addr);
    proto->cmdline_addr     = hhdm_phys_to_virt(proto->cmdline_addr);
    if (proto->cksum_table)
        proto->cksum_table = hhdm_phys_to_virt(proto->cksum_table);
    if (proto->sclp_info_addr)
        proto->sclp_info_addr = hhdm_phys_to_virt(proto->sclp_info_addr);

    uint64_t v_proto = hhdm_phys_to_virt(boot_proto);
    uint64_t v_stack = proto->kernel_stack_top;

    uint64_t cr0;
    arch_ctl_store(cr0, 0, 0);

    if (has_edat1) {
        cr0 |= (1ULL << (63 - 40));   // CR0.40: Enhanced-DAT enablement
    }

    cr0 &= ~(1ULL << (63 - 29));      // CR0.29: ASCE-type = 0 (primary-space)

    arch_ctl_load(cr0, 0, 0);

    uint64_t asce = (uint64_t)(uintptr_t)r1_table | Z_ASCE_DT_R1 | Z_ASCE_TL_2048;
    arch_ctl_load(asce, 1, 1);
    arch_ctl_load(asce, 13, 13); // Home Space

    proto->pgtbl_pool_end = (pool_next + 4095ULL) & ~4095ULL;

    if (proto->pgtbl_pool_end > map_bytes) {
        proto->hhdm_phys_coverage_end = proto->pgtbl_pool_end;
    } else {
        proto->hhdm_phys_coverage_end = map_bytes;
    }
    proto->cr1_snapshot = asce;
    proto->cr13_snapshot = asce;
    proto->cr0_snapshot = cr0;

    arch_set_prefix((uint32_t)bsp_lowcore_phys);

    __asm__ volatile("ptlb" ::: "memory");

    // PSW_MASK_KERNEL_DAT = PSW_BIT_DAT | PSW_BIT_EA | PSW_BIT_BA
    // = 0x0400000180000000.  Using the named constant instead of a literal
    // ensures this stays in sync with the rest of the kernel's PSW definitions.
    static uint64_t jump_psw[2] __attribute__((aligned(16)));
    jump_psw[0] = PSW_MASK_KERNEL_DAT;
    jump_psw[1] = entry;

    __asm__ volatile(
        "lgr   %%r2, %[proto]\n"
        "lgr   %%r15, %[stack]\n"
        "lpswe %[psw]\n"
        :
        : [proto] "r"(v_proto),
          [stack] "r"(v_stack),
          [psw]   "Q"(jump_psw)
        : "r2", "memory"
    );
    __builtin_unreachable();
}
