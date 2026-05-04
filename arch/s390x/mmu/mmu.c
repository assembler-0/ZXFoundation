// SPDX-License-Identifier: Apache-2.0
// arch/s390x/mmu/mmu.c
//
/// @brief z/Architecture 5-level DAT page table manager.

#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/mmu.h>
#include <arch/s390x/cpu/features.h>

static mmu_pgtbl_t kernel_pgtbl;
static bool        mmu_ready    = false;
/// True when CR0 bit 40 (Enhanced-DAT 1) is set — required for FC=1 STEs.
static bool        edat1_enabled = false;

/// @brief Allocate a zero-filled 16 KB table from the PMM (order=2, 4 pages).
/// @return Physical address of the table, or 0 on failure.
static uint64_t alloc_table_page(void) {
    zx_page_t *p = pmm_alloc_pages(2, ZX_GFP_NORMAL | ZX_GFP_DMA_FALLBACK | ZX_GFP_ZERO);
    if (!p) return 0;
    return pmm_page_to_phys(p);
}

/// @brief Allocate a zero-filled 4 KB page table from the PMM (order=0).
/// @return Physical address of the page table, or 0 on failure.
static uint64_t alloc_pt_page(void) {
    zx_page_t *p = pmm_alloc_page(ZX_GFP_NORMAL | ZX_GFP_DMA_FALLBACK | ZX_GFP_ZERO);
    if (!p) return 0;
    return pmm_page_to_phys(p);
}

/// @brief Allocate a 16 KB R1 table (4 contiguous pages, order=2, 16 KB aligned).
/// @return Physical address, or 0 on failure.
static uint64_t alloc_r1_table(void) {
    zx_page_t *page = pmm_alloc_pages(2, ZX_GFP_NORMAL | ZX_GFP_DMA_FALLBACK | ZX_GFP_ZERO);
    if (!page) return 0;
    return pmm_page_to_phys(page);
}


/// @brief Retrieve or create the R2 table pointer from an R1 entry.
static uint64_t *get_or_alloc_r2(uint64_t *r1, uint32_t rfx) {
    uint64_t e = r1[rfx];
    if (e & Z_I_BIT) {
        uint64_t r2_phys = alloc_table_page();
        if (!r2_phys) return nullptr;
        uint64_t *r2 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r2_phys);
        for (int i = 0; i < 2048; i++)
            r2[i] = Z_I_BIT | Z_TL_2048 | Z_TT_R2;
        r1[rfx] = r2_phys | Z_TL_2048 | Z_TT_R1;
        return r2;
    }
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
    if (e & Z_STE_FC) return nullptr; // large page — no PT beneath it
    uint64_t seg_phys = e & ~0xFFFULL;
    return (uint64_t *)(uintptr_t)hhdm_phys_to_virt(seg_phys);
}

static uint64_t *get_or_alloc_pt(uint64_t *seg, uint32_t sx) {
    uint64_t e = seg[sx];
    // If segment entry is invalid, allocate a page table.
    if (e & Z_I_BIT) {
        // Page tables are 4KB (order=0): 256 entries × 8 bytes = 2KB, no TL field.
        uint64_t pt_phys = alloc_pt_page();
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
        pmm_free_pages(phys_to_page(seg_phys), 2);
    }
    pmm_free_pages(phys_to_page((uint64_t)(uintptr_t)hhdm_virt_to_phys((uint64_t)(uintptr_t)r3)), 2);
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
        // R2 table is 16KB (order=2).
        pmm_free_pages(phys_to_page(r2_phys), 2);
    }
    // R1 is 16KB (order=2).
    pmm_free_pages(phys_to_page(pgtbl.r1_phys), 2);
}

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

    if (!(old_pte & Z_PTE_I))
        mmu_ipte(va);

    uint64_t pte = pa; // bits 63:12 = page-frame real address
    if (!(prot & VM_WRITE))
        pte |= Z_PTE_P; // read-only
    pt[px] = pte;

    barrier();
    return 0;
}

int mmu_map_large_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot) {
    if (!edat1_enabled)
        return -1;
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

    if (!(old_ste & Z_I_BIT)) {
        for (int p = 0; p < 256; p++)
            mmu_ipte(va + (uint64_t)p * PAGE_SIZE);
        if (!(old_ste & Z_STE_FC)) {
            uint64_t pt_phys = old_ste & ~0xFFFULL;
            pmm_free_page(phys_to_page(pt_phys));
        }
    }

    uint64_t ste = pa | Z_STE_FC | Z_TT_SEG;
    if (!(prot & VM_WRITE))
        ste |= (1ULL << 8); // STE protected bit (PoP §3.5)
    seg[sx] = ste;

    __asm__ volatile("" ::: "memory");
    return 0;
}

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
        seg[va_sx(va)] = Z_I_BIT | Z_TT_SEG;
        uint64_t base = va & ~0xFFFFFULL;
        for (int p = 0; p < 256; p++)
            mmu_ipte(base + (uint64_t)p * PAGE_SIZE);
        return;
    }

    uint64_t *pt = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(se & ~0xFFFULL);
    uint32_t px  = va_px(va);
    if (pt[px] & Z_PTE_I) return; // already invalid

    mmu_ipte(va);
    pt[px] = Z_PTE_I;
}

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
        uint64_t frame = se & ~0xFFFFFULL;
        return frame | va_bx(va) | ((uint64_t)va_px(va) << PAGE_SHIFT);
    }

    const uint64_t *pt  = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(se & ~0xFFFULL);
    uint64_t pte = pt[va_px(va)];
    if (pte & Z_PTE_I) return ~0ULL;
    // PTE bits 63:12 = page-frame real address.
    return (pte & PAGE_MASK) | va_bx(va);
}

bool mmu_is_large_page(const mmu_pgtbl_t *pgtbl, uint64_t va) {
    const uint64_t *r1 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);

    uint64_t e1 = r1[va_rfx(va)];
    if (e1 & Z_I_BIT) return false;
    const uint64_t *r2 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(e1 & ~0xFFFULL);

    uint64_t e2 = r2[va_rsx(va)];
    if (e2 & Z_I_BIT) return false;
    const uint64_t *r3 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(e2 & ~0xFFFULL);

    uint64_t e3 = r3[va_rtx(va)];
    if (e3 & Z_I_BIT) return false;
    const uint64_t *seg = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(e3 & ~0xFFFULL);

    uint64_t se = seg[va_sx(va)];
    if (se & Z_I_BIT) return false;
    return (se & Z_STE_FC) != 0;
}

void mmu_load_pgtbl(const mmu_pgtbl_t *pgtbl) {
    __asm__ volatile(
        "lctlg 1,1,%0\n"
        "ptlb\n"         // purge local TLB after ASCE change
        :: "Q"(pgtbl->asce) : "memory"
    );
}

void mmu_init() {
    uint64_t cr1;
    __asm__ volatile("stctg 1,1,%0" : "=Q"(cr1));

    kernel_pgtbl.asce    = cr1;
    kernel_pgtbl.r1_phys = cr1 & ~0xFFFULL;

    edat1_enabled = arch_cpu_has_sys_feature(ZX_SYS_FEATURE_EDAT1);

    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(kernel_pgtbl.r1_phys);
    uint32_t scrubbed = 0;
    for (int i = 0; i < 2048; i++) {
        uint64_t e = r1[i];
        if (e & Z_I_BIT) continue;
        uint64_t r2_phys = e & ~0xFFFULL;
        if (r2_phys == 0 || r2_phys >= (4ULL * 1024 * 1024 * 1024)) {
            r1[i] = Z_I_BIT | Z_TL_2048 | Z_TT_R1;
            scrubbed++;
        }
    }
    if (scrubbed > 0)
        mmu_flush_tlb_local();

    mmu_ready = true;
    printk("mmu: ASCE=%016llx R1=%016llx EDAT-1=%s (scrubbed %u R1 entries)\n",
           (unsigned long long)kernel_pgtbl.asce,
           (unsigned long long)kernel_pgtbl.r1_phys,
           edat1_enabled ? "on" : "off",
           scrubbed);
}

mmu_pgtbl_t *mmu_kernel_pgtbl(void) {
    return &kernel_pgtbl;
}

bool mmu_has_edat1(void) {
    return edat1_enabled;
}