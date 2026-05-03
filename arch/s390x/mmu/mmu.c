// SPDX-License-Identifier: Apache-2.0
// arch/s390x/mmu/mmu.c
//
/// @brief Kernel-side z/Architecture 5-level DAT page table manager.
///
///        DESIGN
///        ======
///        This module owns the kernel's permanent page-table hierarchy
///        (kernel_pgtbl) and provides the functions to manipulate it
///        and any future per-process hierarchies.
///
///        The kernel's ASCE was built by the ZXFL bootloader (mmu.c in
///        arch/s390x/init/zxfl/common/) and loaded into CR1 before
///        control reached start_kernel().  Here we reconstruct a
///        mmu_pgtbl_t handle from the CR1 snapshot in the boot protocol,
///        so the kernel can extend and modify the live hierarchy.
///
///        TABLE ALLOCATION
///        ================
///        All subordinate tables (R2, R3, Segment, Page) are exactly 4 KB
///        so they map directly to one PMM page each.  We allocate with
///        ZX_GFP_DMA to guarantee the table origin fits in a 32-bit real
///        address (required by the PoP for table-origin fields < 4 GB on
///        real hardware; QEMU/Hercules do not enforce this, but we do for
///        correctness).
///
///        The R1 table is 16 KB (2048 × 8 bytes) and 16 KB-aligned.
///        We allocate 4 contiguous order-2 pages from the DMA zone.
///
///        SMP TLB COHERENCY
///        =================
///        mmu_map_page():   writes the PTE then does nothing extra —
///          a new mapping is invisible until the instruction that uses the
///          new VA, so no flush is needed for the installing CPU.  Other
///          CPUs never see the VA as valid until after the store, so no
///          shootdown is needed for new mappings either.
///
///        mmu_unmap_page(): uses IPTE (Invalidate Page Table Entry) which
///          is a serialising, hardware-broadcast instruction: the microcode
///          removes the TLB entry on ALL CPUs sharing the primary ASCE
///          before the instruction completes.  No software IPI required.

#include <arch/s390x/mmu.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <lib/string.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static mmu_pgtbl_t kernel_pgtbl;
static bool        mmu_ready = false;

// ---------------------------------------------------------------------------
// Internal: table allocators
// ---------------------------------------------------------------------------

/// @brief Allocate a zero-filled 4 KB table from the PMM (DMA zone).
/// @return Physical address of the table, or 0 on failure.
static uint64_t alloc_table_page(void) {
    zx_page_t *page = pmm_alloc_page(ZX_GFP_DMA | ZX_GFP_ZERO);
    if (!page) return 0;
    return pmm_page_to_phys(page);
}

/// @brief Allocate a 16 KB R1 table (4 contiguous pages, order=2, 16 KB aligned).
/// @return Physical address, or 0 on failure.
static uint64_t alloc_r1_table(void) {
    // order=2 gives 4 pages = 16 KB; the PMM guarantees natural power-of-2
    // alignment for buddy blocks, so a 4-page block is 16 KB-aligned.
    zx_page_t *page = pmm_alloc_pages(2, ZX_GFP_DMA | ZX_GFP_ZERO);
    if (!page) return 0;
    return pmm_page_to_phys(page);
}

// ---------------------------------------------------------------------------
// Internal: table-entry walk (allocating on demand)
// ---------------------------------------------------------------------------

/// @brief Retrieve or create the R2 table pointer from an R1 entry.
static uint64_t *get_or_alloc_r2(uint64_t *r1, uint32_t rfx) {
    uint64_t e = r1[rfx];
    if (e & Z_I_BIT) {
        // R1 entry is invalid — allocate a new R2 table.
        uint64_t r2_phys = alloc_table_page();
        if (!r2_phys) return nullptr;
        // Initialise all R2 entries to invalid.
        uint64_t *r2 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r2_phys);
        for (int i = 0; i < 2048; i++)
            r2[i] = Z_I_BIT | Z_TL_2048 | Z_TT_R2;
        // Write the R1 entry: origin | TT_R1 | TL_2048.
        r1[rfx] = r2_phys | Z_TL_2048 | Z_TT_R1;
        return r2;
    }
    // Valid entry: mask off the TT/TL flags to get the R2 physical origin.
    uint64_t r2_phys = e & ~0xFFFULL;  // bits 10:0 are flags
    return (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r2_phys);
}

static uint64_t *get_or_alloc_r3(uint64_t *r2, uint32_t rsx) {
    uint64_t e = r2[rsx];
    if (e & Z_I_BIT) {
        uint64_t r3_phys = alloc_table_page();
        if (!r3_phys) return nullptr;
        uint64_t *r3 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r3_phys);
        for (int i = 0; i < 2048; i++)
            r3[i] = Z_I_BIT | Z_TL_2048 | Z_TT_R3;
        r2[rsx] = r3_phys | Z_TL_2048 | Z_TT_R2;
        return r3;
    }
    uint64_t r3_phys = e & ~0xFFFULL;
    return (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r3_phys);
}

static uint64_t *get_or_alloc_seg(uint64_t *r3, uint32_t rtx) {
    uint64_t e = r3[rtx];
    if (e & Z_I_BIT) {
        uint64_t seg_phys = alloc_table_page();
        if (!seg_phys) return nullptr;
        uint64_t *seg = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(seg_phys);
        for (int i = 0; i < 2048; i++)
            seg[i] = Z_I_BIT | Z_TT_SEG;
        r3[rtx] = seg_phys | Z_TL_2048 | Z_TT_R3;
        return seg;
    }
    // Check if this is already a large-page STE (FC=1).
    if (e & Z_STE_FC) return nullptr; // large page — no PT beneath it
    uint64_t seg_phys = e & ~0xFFFULL;
    return (uint64_t *)(uintptr_t)hhdm_phys_to_virt(seg_phys);
}

static uint64_t *get_or_alloc_pt(uint64_t *seg, uint32_t sx) {
    uint64_t e = seg[sx];
    // If segment entry is invalid, allocate a page table.
    if (e & Z_I_BIT) {
        uint64_t pt_phys = alloc_table_page();
        if (!pt_phys) return nullptr;
        uint64_t *pt = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pt_phys);
        for (int i = 0; i < 256; i++)
            pt[i] = Z_PTE_I;
        seg[sx] = pt_phys | Z_TT_SEG;  // valid STE pointing at PT
        return pt;
    }
    if (e & Z_STE_FC) return nullptr;  // large page — no PT
    uint64_t pt_phys = e & ~0xFFFULL;
    return (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pt_phys);
}

// ---------------------------------------------------------------------------
// mmu_pgtbl_alloc / free
// ---------------------------------------------------------------------------

mmu_pgtbl_t mmu_pgtbl_alloc(void) {
    mmu_pgtbl_t h = {0, 0};
    uint64_t r1_phys = alloc_r1_table();
    if (!r1_phys) return h;

    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r1_phys);
    // Mark all 2048 R1 entries invalid.
    for (int i = 0; i < 2048; i++)
        r1[i] = Z_I_BIT | Z_TL_2048 | Z_TT_R1;

    h.r1_phys = r1_phys;
    h.asce    = r1_phys | Z_ASCE_DT_R1 | Z_ASCE_TL_2048;
    return h;
}

/// @brief Recursively free one level of the DAT hierarchy.
///        We do NOT recurse into large-page STEs (FC=1) — those have no PT.
static void free_r3_subtree(uint64_t *r3) {
    for (int rtx = 0; rtx < 2048; rtx++) {
        uint64_t e = r3[rtx];
        if (e & Z_I_BIT) continue;
        if (e & Z_STE_FC) continue; // large page STE, no subtable

        uint64_t seg_phys = e & ~0xFFFULL;
        uint64_t *seg = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(seg_phys);
        for (int sx = 0; sx < 2048; sx++) {
            uint64_t se = seg[sx];
            if (se & Z_I_BIT) continue;
            if (se & Z_STE_FC) continue;
            uint64_t pt_phys = se & ~0xFFFULL;
            pmm_free_page(phys_to_page(pt_phys));
        }
        pmm_free_page(phys_to_page(seg_phys));
    }
    pmm_free_page(phys_to_page((uint64_t)(uintptr_t)hhdm_virt_to_phys((uint64_t)(uintptr_t)r3)));
}

void mmu_pgtbl_free(mmu_pgtbl_t pgtbl) {
    if (!pgtbl.r1_phys) return;
    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl.r1_phys);
    for (int rfx = 0; rfx < 2048; rfx++) {
        uint64_t e1 = r1[rfx];
        if (e1 & Z_I_BIT) continue;
        uint64_t r2_phys = e1 & ~0xFFFULL;
        uint64_t *r2 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r2_phys);
        for (int rsx = 0; rsx < 2048; rsx++) {
            uint64_t e2 = r2[rsx];
            if (e2 & Z_I_BIT) continue;
            uint64_t r3_phys = e2 & ~0xFFFULL;
            uint64_t *r3 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r3_phys);
            free_r3_subtree(r3);
        }
        pmm_free_page(phys_to_page(r2_phys));
    }
    // R1 is 4 pages (order=2).
    pmm_free_pages(phys_to_page(pgtbl.r1_phys), 2);
}

// ---------------------------------------------------------------------------
// mmu_map_page
// ---------------------------------------------------------------------------

int mmu_map_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot) {
    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);

    uint64_t *r2 = get_or_alloc_r2(r1, va_rfx(va));
    if (!r2) return -1;

    uint64_t *r3 = get_or_alloc_r3(r2, va_rsx(va));
    if (!r3) return -1;

    uint64_t *seg = get_or_alloc_seg(r3, va_rtx(va));
    if (!seg) return -1;

    uint64_t *pt = get_or_alloc_pt(seg, va_sx(va));
    if (!pt) return -1;

    uint32_t px = va_px(va);
    uint64_t old_pte = pt[px];

    // If the old PTE was valid, issue IPTE before overwriting to ensure no
    // CPU retains a stale TLB entry for this VA.  The hardware will stall
    // all CPUs that hold a matching entry until the IPTE completes.
    if (!(old_pte & Z_PTE_I))
        mmu_ipte(va);

    uint64_t pte = pa; // bits 63:12 = page-frame real address
    if (!(prot & VM_WRITE))
        pte |= Z_PTE_P; // read-only
    // VM_EXEC has no separate PTE bit on s390x base DAT; NX is per-segment.
    pt[px] = pte;

    // A compiler barrier ensures the PTE write is ordered before any
    // subsequent instruction that might use the new mapping.
    __asm__ volatile("" ::: "memory");
    return 0;
}

// ---------------------------------------------------------------------------
// mmu_map_large_page (EDAT-1, 1 MB STE with FC=1)
// ---------------------------------------------------------------------------

int mmu_map_large_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot) {
    // Verify 1 MB alignment.
    if (va & 0xFFFFFULL) panic("mmu_map_large_page: va not 1 MB aligned");
    if (pa & 0xFFFFFULL) panic("mmu_map_large_page: pa not 1 MB aligned");

    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);

    uint64_t *r2 = get_or_alloc_r2(r1, va_rfx(va));
    if (!r2) return -1;

    uint64_t *r3 = get_or_alloc_r3(r2, va_rsx(va));
    if (!r3) return -1;

    uint64_t *seg = get_or_alloc_seg(r3, va_rtx(va));
    if (!seg) return -1;

    uint32_t sx = va_sx(va);
    uint64_t old_ste = seg[sx];

    // Invalidate any existing sub-page mappings before installing the large STE.
    if (!(old_ste & Z_I_BIT)) {
        // Existing mapping: issue IPTE for each 4 KB sub-page.
        for (int p = 0; p < 256; p++)
            mmu_ipte(va + (uint64_t)p * PAGE_SIZE);
        // If the old STE pointed at a page table, free it.
        if (!(old_ste & Z_STE_FC)) {
            uint64_t pt_phys = old_ste & ~0xFFFULL;
            pmm_free_page(phys_to_page(pt_phys));
        }
    }

    // Write the large-page STE: physical address | FC=1 | TT=SEG.
    uint64_t ste = pa | Z_STE_FC | Z_TT_SEG;
    if (!(prot & VM_WRITE))
        ste |= (1ULL << 8); // STE protected bit (PoP §3.5)
    seg[sx] = ste;

    __asm__ volatile("" ::: "memory");
    return 0;
}

// ---------------------------------------------------------------------------
// mmu_unmap_page
// ---------------------------------------------------------------------------

void mmu_unmap_page(mmu_pgtbl_t *pgtbl, uint64_t va) {
    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);

    uint64_t e1 = r1[va_rfx(va)];
    if (e1 & Z_I_BIT) return;
    uint64_t *r2 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(e1 & ~0xFFFULL);

    uint64_t e2 = r2[va_rsx(va)];
    if (e2 & Z_I_BIT) return;
    uint64_t *r3 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(e2 & ~0xFFFULL);

    uint64_t e3 = r3[va_rtx(va)];
    if (e3 & Z_I_BIT) return;
    uint64_t *seg = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(e3 & ~0xFFFULL);

    uint64_t se = seg[va_sx(va)];
    if (se & Z_I_BIT) return;
    if (se & Z_STE_FC) {
        // Large page: invalidate segment entry.
        seg[va_sx(va)] = Z_I_BIT | Z_TT_SEG;
        // Issue IPTE for each constituent 4 KB sub-page.
        uint64_t base = va & ~0xFFFFFULL;
        for (int p = 0; p < 256; p++)
            mmu_ipte(base + (uint64_t)p * PAGE_SIZE);
        return;
    }

    uint64_t *pt = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(se & ~0xFFFULL);
    uint32_t px  = va_px(va);
    if (pt[px] & Z_PTE_I) return; // already invalid

    // IPTE performs the hardware-broadcast invalidation across all CPUs before
    // this instruction retires.  We MUST call it before writing Z_PTE_I,
    // because IPTE atomically marks the PTE invalid AND shoots down TLBs.
    // Writing the PTE manually first would leave a window where another CPU's
    // speculative prefetch could see the half-updated entry.
    mmu_ipte(va);
    // IPTE has already written Z_PTE_I into the PTE on z/Architecture;
    // the explicit write below is redundant but kept for clarity/portability.
    pt[px] = Z_PTE_I;
}

// ---------------------------------------------------------------------------
// mmu_virt_to_phys — software page-table walk
// ---------------------------------------------------------------------------

uint64_t mmu_virt_to_phys(const mmu_pgtbl_t *pgtbl, uint64_t va) {
    const uint64_t *r1 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);

    uint64_t e1 = r1[va_rfx(va)];
    if (e1 & Z_I_BIT) return ~0ULL;
    const uint64_t *r2 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(e1 & ~0xFFFULL);

    uint64_t e2 = r2[va_rsx(va)];
    if (e2 & Z_I_BIT) return ~0ULL;
    const uint64_t *r3 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(e2 & ~0xFFFULL);

    uint64_t e3 = r3[va_rtx(va)];
    if (e3 & Z_I_BIT) return ~0ULL;
    const uint64_t *seg = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(e3 & ~0xFFFULL);

    uint64_t se = seg[va_sx(va)];
    if (se & Z_I_BIT) return ~0ULL;
    if (se & Z_STE_FC) {
        // 1 MB large page: frame origin in bits 63:20.
        uint64_t frame = se & ~0xFFFFFULL;
        return frame | va_bx(va) | ((uint64_t)va_px(va) << PAGE_SHIFT);
    }

    const uint64_t *pt  = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(se & ~0xFFFULL);
    uint64_t pte = pt[va_px(va)];
    if (pte & Z_PTE_I) return ~0ULL;
    // PTE bits 63:12 = page-frame real address.
    return (pte & PAGE_MASK) | va_bx(va);
}

// ---------------------------------------------------------------------------
// mmu_load_pgtbl — install ASCE into CR1
// ---------------------------------------------------------------------------

void mmu_load_pgtbl(const mmu_pgtbl_t *pgtbl) {
    // lctlg 1,1 loads CR1 with the new ASCE.
    __asm__ volatile(
        "lctlg 1,1,%0\n"
        "ptlb\n"         // purge local TLB after ASCE change
        :: "Q"(pgtbl->asce) : "memory"
    );
}

// ---------------------------------------------------------------------------
// mmu_init — reconstruct the kernel pgtbl handle from the live CR1
// ---------------------------------------------------------------------------

void mmu_init(void) {
    uint64_t cr1;
    __asm__ volatile("stctg 1,1,%0" : "=Q"(cr1));

    // The bootloader placed the R1 table origin in CR1[63:12] with
    // DT=11 and TL=11 in bits 3:0 and 1:0 respectively (PoP §4.2.4).
    // Strip the flag bits to recover the physical R1 origin.
    kernel_pgtbl.asce    = cr1;
    kernel_pgtbl.r1_phys = cr1 & ~0xFFFULL; // bits 63:12 = table origin

    // --- CRITICAL: Scrub unused R1 entries ---
    // The bootloader only populated R1 entries for the HHDM mapping and
    // left the remaining 2048 entries uninitialised.  An all-zero entry
    // (or random garbage) has Z_I_BIT (bit 32) clear, which makes
    // get_or_alloc_r2() treat it as a valid R2 pointer — dereferencing
    // a wild address and causing a Region-second-translation exception.
    //
    // Walk the entire R1 table: any entry that is NOT marked invalid
    // AND whose table-origin does not point within physical RAM is
    // forced to invalid.  We also invalidate any truly-zero entries
    // (a valid R2 table at physical 0x0 is impossible — that's lowcore).
    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(kernel_pgtbl.r1_phys);
    uint32_t scrubbed = 0;
    for (int i = 0; i < 2048; i++) {
        uint64_t e = r1[i];
        if (e & Z_I_BIT)
            continue;  // already invalid — fine
        // Extract the purported R2 table origin.
        uint64_t r2_phys = e & ~0xFFFULL;
        // Sanity: a valid R2 origin must be page-aligned, non-zero,
        // and within the first 512 MB (or whatever max phys is).
        // We use a conservative 4 GB bound for DMA-zone tables.
        if (r2_phys == 0 || r2_phys >= (4ULL * 1024 * 1024 * 1024)) {
            r1[i] = Z_I_BIT | Z_TL_2048 | Z_TT_R1;
            scrubbed++;
        }
    }

    // Flush the local TLB after modifying live R1 entries.
    if (scrubbed > 0)
        mmu_flush_tlb_local();

    mmu_ready = true;
    printk("mmu: kernel ASCE = %016llx (R1 phys = %016llx, scrubbed %u stale R1 entries)\n",
           (unsigned long long)kernel_pgtbl.asce,
           (unsigned long long)kernel_pgtbl.r1_phys,
           scrubbed);
}

mmu_pgtbl_t *mmu_kernel_pgtbl(void) {
    return &kernel_pgtbl;
}