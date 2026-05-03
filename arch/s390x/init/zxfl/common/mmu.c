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

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <zxfoundation/zconfig.h>

// ---------------------------------------------------------------------------
// z/Architecture paging constants (PoP SA22-7832, Chapter 3)
// ---------------------------------------------------------------------------

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

/// Segment Table Entry: Format-Control bit (bit 54).
/// With EDAT-1, the STE directly maps a 1 MB large page.
#define Z_STE_FC             0x200ULL

/// Page Table Entry: Invalid bit (bit 53).
#define Z_PTE_I              0x400ULL

/// STFLE facility bit for EDAT-1 (enhanced DAT 1).
#define STFLE_BIT_EDAT1      78U

/// Bytes per 1 MB segment.
#define SEG_SIZE             (1024ULL * 1024ULL)

/// Entries per segment table.
#define SEG_TABLE_ENTRIES    2048U

/// Bytes covered by one segment table (2 GB).
#define SEG_TABLE_COVERAGE   ((uint64_t)SEG_TABLE_ENTRIES * SEG_SIZE)

/// Entries per page table.
#define PAGE_TABLE_ENTRIES   256U

// ---------------------------------------------------------------------------
// Static tables for the top 3 levels (R1, R2, R3).
//
// One of each is sufficient:
//   R1: identity uses entry 0, HHDM uses entry 2047.
//   R2: shared — identity uses entry 0, HHDM uses entry 2016.
//       These indices never overlap, so sharing one R2 table is safe.
//   R3: shared — both identity and HHDM map the same physical RAM.
//
// Lower levels (segment tables, page tables) are dynamically allocated
// from a bump pool above the kernel image.
// ---------------------------------------------------------------------------

static uint64_t r1_table[2048] __attribute__((aligned(16384)));
static uint64_t r2_table[2048] __attribute__((aligned(16384)));
static uint64_t r3_table[2048] __attribute__((aligned(16384)));

/// @brief Linear bump allocator for page-table memory.
static uint64_t pool_next = 0;

/// @brief Allocate 'size' bytes from the pool, aligned to 'align'.
static uint64_t pool_alloc(uint64_t size, uint64_t align) {
    pool_next = (pool_next + align - 1) & ~(align - 1);
    uint64_t addr = pool_next;
    pool_next += size;
    return addr;
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
///
///        The loader ONLY accepts kernels with a higher-half entry point
///        (e_entry >= CONFIG_KERNEL_VIRT_OFFSET).  This is enforced in
///        elfload.c; by the time we reach here, 'entry' is guaranteed to
///        be an HHDM virtual address.
///
/// @param entry      Kernel entry point (HHDM virtual address from ELF e_entry).
/// @param boot_proto Physical address of the zxfl_boot_protocol_t struct.
[[noreturn]] void zxfl_mmu_setup_and_jump(uint64_t entry, uint64_t boot_proto) {
    zxfl_boot_protocol_t *proto = (zxfl_boot_protocol_t *)boot_proto;
    const uint64_t virt_base = CONFIG_KERNEL_VIRT_OFFSET;

    const bool has_edat1 = stfle_has_facility(proto->stfle_fac, STFLE_BIT_EDAT1);

    // -----------------------------------------------------------------------
    // 1. Determine how much physical memory to map.
    //    Cap at 4 TB — one R3 table's worth.
    // -----------------------------------------------------------------------
    uint64_t map_bytes = proto->mem_total_bytes;
    if (map_bytes < 0x200000ULL) map_bytes = 0x200000ULL;
    map_bytes = (map_bytes + SEG_SIZE - 1) & ~(SEG_SIZE - 1);

    const uint64_t max_map = 2048ULL * SEG_TABLE_COVERAGE;
    if (map_bytes > max_map) map_bytes = max_map;

    const uint64_t total_segments = map_bytes / SEG_SIZE;
    const uint32_t num_seg_tables = (uint32_t)((total_segments + SEG_TABLE_ENTRIES - 1)
                                                / SEG_TABLE_ENTRIES);

    // -----------------------------------------------------------------------
    // 2. Initialize R1, R2, R3 tables to Invalid with correct TT bits.
    // -----------------------------------------------------------------------
    for (uint32_t i = 0; i < 2048; i++) {
        r1_table[i] = Z_I | Z_TL_2048 | Z_TT_R1;
        r2_table[i] = Z_I | Z_TL_2048 | Z_TT_R2;
        r3_table[i] = Z_I | Z_TL_2048 | Z_TT_R3;
    }

    // -----------------------------------------------------------------------
    // 3. Set up the table pool above the kernel image.
    // -----------------------------------------------------------------------
    uint64_t pool_base = (proto->kernel_phys_end + 0xFFFFFULL) & ~0xFFFFFULL;
    if (pool_base < 0x2000000ULL) pool_base = 0x2000000ULL;
    pool_next = pool_base;

    if (!has_edat1) {
        print("zxfl: EDAT-1 missing, using 4KB page fallback\n");
    }

    // -----------------------------------------------------------------------
    // 4. Allocate and populate segment tables.
    // -----------------------------------------------------------------------
    for (uint32_t st = 0; st < num_seg_tables; st++) {
        uint64_t *seg_table = alloc_seg_table();

        uint64_t seg_start = (uint64_t)st * SEG_TABLE_ENTRIES;
        uint64_t seg_end   = seg_start + SEG_TABLE_ENTRIES;
        if (seg_end > total_segments) seg_end = total_segments;

        for (uint64_t s = seg_start; s < seg_end; s++) {
            uint64_t phys = s * SEG_SIZE;
            uint32_t sx   = (uint32_t)(s % SEG_TABLE_ENTRIES);

            if (has_edat1) {
                seg_table[sx] = phys | Z_STE_FC | Z_TT_SEG;
            } else {
                uint64_t *pt = alloc_page_table();
                for (uint32_t p = 0; p < PAGE_TABLE_ENTRIES; p++) {
                    pt[p] = phys + ((uint64_t)p * 4096ULL);
                }
                seg_table[sx] = (uint64_t)(uintptr_t)pt | Z_TT_SEG;
            }
        }

        // Wire into R3.
        r3_table[st] = (uint64_t)(uintptr_t)seg_table | Z_TL_2048 | Z_TT_R3;
    }

    // -----------------------------------------------------------------------
    // 5. Wire the 5-level hierarchy.
    //
    //    Virtual address decomposition (CONFIG_KERNEL_VIRT_OFFSET = 0xFFFF800000000000):
    //      RFX = (addr >> 53) & 0x7FF
    //      RSX = (addr >> 42) & 0x7FF
    //
    //    Identity map (VA = 0x0):
    //      RFX = 0,    RSX = 0    → R1[0] → R2, R2[0] → R3
    //    HHDM map (VA = 0xFFFF800000000000):
    //      RFX = 2047, RSX = 2016 → R1[2047] → R2, R2[2016] → R3
    //
    //    R1[0] and R1[2047] both point to the SAME R2 table.
    //    R2[0] and R2[2016] both point to the SAME R3 table.
    //    This works because the RSX indices don't overlap.
    // -----------------------------------------------------------------------
    uint32_t rfx_identity = 0;
    uint32_t rfx_hhdm     = (uint32_t)((virt_base >> 53) & 0x7FFULL);
    uint32_t rsx_identity = 0;
    uint32_t rsx_hhdm     = (uint32_t)((virt_base >> 42) & 0x7FFULL);

    // R1 → R2 (shared).
    r1_table[rfx_identity] = (uint64_t)(uintptr_t)r2_table | Z_TL_2048 | Z_TT_R1;
    r1_table[rfx_hhdm]     = (uint64_t)(uintptr_t)r2_table | Z_TL_2048 | Z_TT_R1;

    // R2 → R3 (shared).
    r2_table[rsx_identity] = (uint64_t)(uintptr_t)r3_table | Z_TL_2048 | Z_TT_R2;
    r2_table[rsx_hhdm]     = (uint64_t)(uintptr_t)r3_table | Z_TL_2048 | Z_TT_R2;

    // -----------------------------------------------------------------------
    // 6. Convert boot protocol pointers from physical to HHDM virtual.
    // -----------------------------------------------------------------------
    uint64_t v_proto = hhdm_phys_to_virt(boot_proto);
    uint64_t v_stack = hhdm_phys_to_virt(proto->kernel_stack_top);
    proto->kernel_stack_top = v_stack;
    proto->mem_map_addr     = hhdm_phys_to_virt(proto->mem_map_addr);
    proto->cmdline_addr     = hhdm_phys_to_virt(proto->cmdline_addr);

    // -----------------------------------------------------------------------
    // 7. Configure control registers and load the ASCE.
    // -----------------------------------------------------------------------
    uint64_t cr0;
    __asm__ volatile("stctg 0,0,%0" : "=Q"(cr0));

    if (has_edat1) {
        cr0 |= (1ULL << (63 - 40));   // CR0.40: Enhanced-DAT enablement
    }

    cr0 &= ~(1ULL << (63 - 29));      // CR0.29: ASCE-type = 0 (primary-space)

    __asm__ volatile("lctlg 0,0,%0" :: "Q"(cr0));

    // ASCE: Region-First table origin | DT=11 | TL=11.
    uint64_t asce = (uint64_t)(uintptr_t)r1_table | Z_ASCE_DT_R1 | Z_ASCE_TL_2048;
    __asm__ volatile("lctlg 1,1,%0" :: "Q"(asce) : "memory");

    // Purge TLB — required after loading a new ASCE.
    __asm__ volatile("ptlb" ::: "memory");

    // -----------------------------------------------------------------------
    // 8. LPSWE into the kernel (atomically enables DAT + jumps to VMA).
    // -----------------------------------------------------------------------
    static uint64_t jump_psw[2] __attribute__((aligned(16)));
    jump_psw[0] = 0x0404000180000000ULL;   // DAT=1, EA=1, BA=1
    jump_psw[1] = entry;                    // HHDM virtual entry point

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
