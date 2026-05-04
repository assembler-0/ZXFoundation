// SPDX-License-Identifier: Apache-2.0
// arch/s390x/mmu/mmu.c
//
/// @brief z/Architecture 5-level DAT page table manager.

#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <zxfoundation/spinlock.h>
#include <arch/s390x/mmu.h>
#include <arch/s390x/cpu/features.h>
#include <zxfoundation/memory/vmm.h>

static mmu_pgtbl_t kernel_pgtbl;
static bool        edat1_enabled = false;
static bool        edat2_enabled = false;

/// Allocate a zero-filled 16 KB table (order=2, 4 pages) for R1/R2/R3/segment.
static uint64_t alloc_table(void) {
    zx_page_t *p = pmm_alloc_pages(Z_TABLE_ORDER,
                                   ZX_GFP_NORMAL | ZX_GFP_DMA_FALLBACK | ZX_GFP_ZERO);
    return p ? pmm_page_to_phys(p) : 0;
}

/// Free a 16 KB table (order=2).
static void free_table(uint64_t phys) {
    pmm_free_pages(phys_to_page(phys), Z_TABLE_ORDER);
}

/// Allocate a zero-filled 4 KB page table (order=0, 1 page).
static uint64_t alloc_pt(void) {
    zx_page_t *p = pmm_alloc_page(ZX_GFP_NORMAL | ZX_GFP_DMA_FALLBACK | ZX_GFP_ZERO);
    return p ? pmm_page_to_phys(p) : 0;
}

/// Free a 4 KB page table (order=0).
static void free_pt(uint64_t phys) {
    pmm_free_page(phys_to_page(phys));
}

#define ENTRY_ORIGIN(e)  ((e) & ~0xFFFULL)

static void init_r1(uint64_t *r1) {
    for (int i = 0; i < (int)Z_TABLE_ENTRIES; i++)
        r1[i] = Z_I_BIT | Z_TL_2048 | Z_TT_R1;
}

static void init_r2(uint64_t *r2) {
    for (int i = 0; i < (int)Z_TABLE_ENTRIES; i++)
        r2[i] = Z_I_BIT | Z_TL_2048 | Z_TT_R2;
}

static void init_r3(uint64_t *r3) {
    for (int i = 0; i < (int)Z_TABLE_ENTRIES; i++)
        r3[i] = Z_I_BIT | Z_TL_2048 | Z_TT_R3;
}

static void init_seg(uint64_t *seg) {
    for (int i = 0; i < (int)Z_TABLE_ENTRIES; i++)
        seg[i] = Z_I_BIT | Z_TT_SEG;
}

static void init_pt(uint64_t *pt) {
    for (int i = 0; i < (int)Z_PT_ENTRIES; i++)
        pt[i] = Z_PTE_I;
}

/// Walk or allocate the R2 table for R1[rfx].
static uint64_t *get_or_alloc_r2(uint64_t *r1, uint32_t rfx) {
    uint64_t e = r1[rfx];
    if (e & Z_I_BIT) {
        uint64_t phys = alloc_table();
        if (!phys) return nullptr;
        uint64_t *r2 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(phys);
        init_r2(r2);
        r1[rfx] = phys | Z_TL_2048 | Z_TT_R1;
        return r2;
    }
    return (uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e));
}

/// Walk or allocate the R3 table for R2[rsx].
static uint64_t *get_or_alloc_r3(uint64_t *r2, uint32_t rsx) {
    uint64_t e = r2[rsx];
    if (e & Z_I_BIT) {
        uint64_t phys = alloc_table();
        if (!phys) return nullptr;
        uint64_t *r3 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(phys);
        init_r3(r3);
        r2[rsx] = phys | Z_TL_2048 | Z_TT_R2;
        return r3;
    }
    if (e & Z_R3E_FC) return nullptr;
    return (uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e));
}

/// Walk or allocate the segment table for R3[rtx].
/// Returns nullptr if the R3 entry is a 2 GB EDAT-2 large page.
static uint64_t *get_or_alloc_seg(uint64_t *r3, uint32_t rtx) {
    uint64_t e = r3[rtx];
    if (e & Z_I_BIT) {
        uint64_t phys = alloc_table();
        if (!phys) return nullptr;
        uint64_t *seg = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(phys);
        init_seg(seg);
        r3[rtx] = phys | Z_TL_2048 | Z_TT_R3;
        return seg;
    }
    if (e & Z_R3E_FC) return nullptr; /* EDAT-2 large page — no segment table */
    return (uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e));
}

/// Walk or allocate the page table for seg[sx].
/// Returns nullptr if the STE is an EDAT-1 large page.
static uint64_t *get_or_alloc_pt(uint64_t *seg, uint32_t sx) {
    uint64_t e = seg[sx];
    if (e & Z_I_BIT) {
        uint64_t phys = alloc_pt();
        if (!phys) return nullptr;
        uint64_t *pt = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(phys);
        init_pt(pt);
        seg[sx] = phys | Z_TT_SEG;
        return pt;
    }
    if (e & Z_STE_FC) return nullptr; /* EDAT-1 large page — no page table */
    return (uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e));
}

mmu_pgtbl_t mmu_pgtbl_alloc(void) {
    mmu_pgtbl_t h = { SPINLOCK_INIT, 0, 0 };
    uint64_t r1_phys = alloc_table();
    if (!r1_phys) return h;

    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r1_phys);
    init_r1(r1);

    h.r1_phys = r1_phys;
    h.asce    = r1_phys | Z_ASCE_DT_R1 | Z_ASCE_TL_2048;
    return h;
}

/// Free one segment table and all page tables beneath it.
/// seg_phys is the physical address of the 16 KB segment table (order=2).
static void free_seg_subtree(uint64_t seg_phys) {
    uint64_t *seg = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(seg_phys);
    for (int sx = 0; sx < (int)Z_TABLE_ENTRIES; sx++) {
        uint64_t se = seg[sx];
        if (se & Z_I_BIT)  continue;
        if (se & Z_STE_FC) continue; /* EDAT-1 large page — no PT to free */
        free_pt(ENTRY_ORIGIN(se));
    }
    free_table(seg_phys);
}

/// Free one R3 table and all segment/PT tables beneath it.
/// r3_phys is the physical address of the 16 KB R3 table (order=2).
static void free_r3_subtree(uint64_t r3_phys) {
    uint64_t *r3 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r3_phys);
    for (int rtx = 0; rtx < (int)Z_TABLE_ENTRIES; rtx++) {
        uint64_t e = r3[rtx];
        if (e & Z_I_BIT)  continue;
        if (e & Z_R3E_FC) continue; /* EDAT-2 large page — no segment table */
        free_seg_subtree(ENTRY_ORIGIN(e));
    }
    free_table(r3_phys);
}

void mmu_pgtbl_free(mmu_pgtbl_t pgtbl) {
    if (!pgtbl.r1_phys) return;
    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl.r1_phys);
    for (int rfx = 0; rfx < (int)Z_TABLE_ENTRIES; rfx++) {
        uint64_t e1 = r1[rfx];
        if (e1 & Z_I_BIT) continue;
        uint64_t r2_phys = ENTRY_ORIGIN(e1);
        uint64_t *r2 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(r2_phys);
        for (int rsx = 0; rsx < (int)Z_TABLE_ENTRIES; rsx++) {
            uint64_t e2 = r2[rsx];
            if (e2 & Z_I_BIT) continue;
            free_r3_subtree(ENTRY_ORIGIN(e2));
        }
        free_table(r2_phys);
    }
    free_table(pgtbl.r1_phys);
}

int mmu_map_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot) {
    irqflags_t flags;
    spin_lock_irqsave(&pgtbl->lock, &flags);

    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);

    uint64_t *r2 = get_or_alloc_r2(r1, va_rfx(va));
    if (!r2) goto oom;

    uint64_t *r3 = get_or_alloc_r3(r2, va_rsx(va));
    if (!r3) goto oom;

    uint64_t *seg = get_or_alloc_seg(r3, va_rtx(va));
    if (!seg) goto oom;

    uint64_t *pt = get_or_alloc_pt(seg, va_sx(va));
    if (!pt) goto oom;

    uint32_t px = va_px(va);
    if (!(pt[px] & Z_PTE_I))
        mmu_ipte(va);

    uint64_t pte = pa & PAGE_MASK;
    if (!(prot & VM_WRITE))
        pte |= Z_PTE_P;
    pt[px] = pte;

    spin_unlock_irqrestore(&pgtbl->lock, flags);
    return 0;
oom:
    spin_unlock_irqrestore(&pgtbl->lock, flags);
    return -1;
}

int mmu_map_large_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot) {
    if (va & ~Z_EDAT1_PAGE_MASK) {
        printk("mmu: mmu_map_large_page: va not 1 MB aligned");
        return -1;
    }
    if (pa & ~Z_EDAT1_PAGE_MASK) {
        printk("mmu: mmu_map_large_page: pa not 1 MB aligned");
        return -1;
    }

    if (!edat1_enabled) {
        for (uint32_t i = 0; i < Z_EDAT1_NR_PAGES; i++) {
            if (mmu_map_page(pgtbl,
                             va + (uint64_t)i * PAGE_SIZE,
                             pa + (uint64_t)i * PAGE_SIZE,
                             prot) != 0)
                return -1;
        }
        return 0;
    }

    irqflags_t flags;
    spin_lock_irqsave(&pgtbl->lock, &flags);

    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);

    uint64_t *r2 = get_or_alloc_r2(r1, va_rfx(va));
    if (!r2) goto oom;

    uint64_t *r3 = get_or_alloc_r3(r2, va_rsx(va));
    if (!r3) goto oom;

    uint64_t *seg = get_or_alloc_seg(r3, va_rtx(va));
    if (!seg) goto oom;

    uint32_t sx = va_sx(va);
    uint64_t old_ste = seg[sx];

    if (!(old_ste & Z_I_BIT)) {
        if (old_ste & Z_STE_FC) {
            for (uint32_t p = 0; p < Z_EDAT1_NR_PAGES; p++)
                mmu_ipte(va + (uint64_t)p * PAGE_SIZE);
        } else {
            uint64_t pt_phys = ENTRY_ORIGIN(old_ste);
            uint64_t *pt = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pt_phys);
            for (uint32_t p = 0; p < Z_PT_ENTRIES; p++) {
                if (!(pt[p] & Z_PTE_I))
                    mmu_ipte(va + (uint64_t)p * PAGE_SIZE);
            }
            free_pt(pt_phys);
        }
    }

    uint64_t ste = (pa & Z_STE_LARGE_ORIGIN_MASK) | Z_STE_FC | Z_TT_SEG;
    if (!(prot & VM_WRITE))
        ste |= Z_STE_PROTECT;
    seg[sx] = ste;

    spin_unlock_irqrestore(&pgtbl->lock, flags);
    return 0;
oom:
    spin_unlock_irqrestore(&pgtbl->lock, flags);
    return -1;
}

int mmu_map_huge_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot) {
    if (va & ~Z_EDAT2_PAGE_MASK) {
        printk("mmu: mmu_map_huge_page: va not 2 GB aligned");
        return -1;
    }
    if (pa & ~Z_EDAT2_PAGE_MASK) {
        printk("mmu: mmu_map_huge_page: pa not 2 GB aligned");
        return -1;
    }

    if (!edat2_enabled) {
        for (uint32_t i = 0; i < Z_TABLE_ENTRIES; i++) {
            if (mmu_map_large_page(pgtbl,
                                   va + (uint64_t)i * Z_EDAT1_PAGE_SIZE,
                                   pa + (uint64_t)i * Z_EDAT1_PAGE_SIZE,
                                   prot) != 0)
                return -1;
        }
        return 0;
    }

    irqflags_t flags;
    spin_lock_irqsave(&pgtbl->lock, &flags);

    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);

    uint64_t *r2 = get_or_alloc_r2(r1, va_rfx(va));
    if (!r2) goto oom;

    uint32_t rsx = va_rsx(va);
    uint64_t old_r2e = r2[rsx];

    if (!(old_r2e & Z_I_BIT)) {
        if (old_r2e & Z_R3E_FC) {
            for (uint32_t i = 0; i < Z_TABLE_ENTRIES; i++)
                for (uint32_t p = 0; p < Z_EDAT1_NR_PAGES; p++)
                    mmu_ipte(va + (uint64_t)i * Z_EDAT1_PAGE_SIZE
                               + (uint64_t)p * PAGE_SIZE);
        } else {
            free_r3_subtree(ENTRY_ORIGIN(old_r2e));
        }
    }

    uint64_t r3e = (pa & Z_R3E_LARGE_ORIGIN_MASK) | Z_R3E_FC | Z_TT_R3 | Z_TL_2048;
    if (!(prot & VM_WRITE))
        r3e |= Z_R3E_PROTECT;
    r2[rsx] = r3e;

    spin_unlock_irqrestore(&pgtbl->lock, flags);
    return 0;
oom:
    spin_unlock_irqrestore(&pgtbl->lock, flags);
    return -1;
}

void mmu_unmap_page(mmu_pgtbl_t *pgtbl, uint64_t va) {
    irqflags_t flags;
    spin_lock_irqsave(&pgtbl->lock, &flags);

    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);

    uint64_t e1 = r1[va_rfx(va)];
    if (e1 & Z_I_BIT) goto out;
    uint64_t *r2 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e1));

    uint64_t e2 = r2[va_rsx(va)];
    if (e2 & Z_I_BIT) goto out;
    /* EDAT-2 large page at R2 level — invalidate the whole 2 GB range. */
    if (e2 & Z_R3E_FC) {
        uint64_t base = va & Z_EDAT2_PAGE_MASK;
        for (uint32_t i = 0; i < Z_TABLE_ENTRIES; i++)
            for (uint32_t p = 0; p < Z_EDAT1_NR_PAGES; p++)
                mmu_ipte(base + (uint64_t)i * Z_EDAT1_PAGE_SIZE
                              + (uint64_t)p * PAGE_SIZE);
        r2[va_rsx(va)] = Z_I_BIT | Z_TL_2048 | Z_TT_R3;
        goto out;
    }
    uint64_t *r3 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e2));

    uint64_t e3 = r3[va_rtx(va)];
    if (e3 & Z_I_BIT) goto out;
    if (e3 & Z_R3E_FC) {
        uint64_t base = va & Z_EDAT2_PAGE_MASK;
        for (uint32_t i = 0; i < Z_TABLE_ENTRIES; i++)
            for (uint32_t p = 0; p < Z_EDAT1_NR_PAGES; p++)
                mmu_ipte(base + (uint64_t)i * Z_EDAT1_PAGE_SIZE
                              + (uint64_t)p * PAGE_SIZE);
        r3[va_rtx(va)] = Z_I_BIT | Z_TL_2048 | Z_TT_R3;
        goto out;
    }
    uint64_t *seg = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e3));

    uint64_t se = seg[va_sx(va)];
    if (se & Z_I_BIT) goto out;
    if (se & Z_STE_FC) {
        uint64_t base = va & Z_EDAT1_PAGE_MASK;
        for (uint32_t p = 0; p < Z_EDAT1_NR_PAGES; p++)
            mmu_ipte(base + (uint64_t)p * PAGE_SIZE);
        seg[va_sx(va)] = Z_I_BIT | Z_TT_SEG;
        goto out;
    }

    uint64_t *pt = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(se));
    uint32_t px  = va_px(va);
    if (pt[px] & Z_PTE_I) goto out;
    mmu_ipte(va);
    pt[px] = Z_PTE_I;

out:
    spin_unlock_irqrestore(&pgtbl->lock, flags);
}

uint64_t mmu_virt_to_phys(const mmu_pgtbl_t *pgtbl, uint64_t va) {
    const uint64_t *r1 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);

    uint64_t e1 = r1[va_rfx(va)];
    if (e1 & Z_I_BIT) return ~0ULL;
    const uint64_t *r2 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e1));

    uint64_t e2 = r2[va_rsx(va)];
    if (e2 & Z_I_BIT) return ~0ULL;
    /* EDAT-2 2 GB large page stored in R2 slot. */
    if (e2 & Z_R3E_FC) {
        uint64_t frame = e2 & Z_R3E_LARGE_ORIGIN_MASK;
        return frame | (va & ~Z_EDAT2_PAGE_MASK);
    }
    const uint64_t *r3 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e2));

    uint64_t e3 = r3[va_rtx(va)];
    if (e3 & Z_I_BIT) return ~0ULL;
    /* EDAT-2 2 GB large page stored in R3 slot. */
    if (e3 & Z_R3E_FC) {
        uint64_t frame = e3 & Z_R3E_LARGE_ORIGIN_MASK;
        return frame | (va & ~Z_EDAT2_PAGE_MASK);
    }
    const uint64_t *seg = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e3));

    uint64_t se = seg[va_sx(va)];
    if (se & Z_I_BIT) return ~0ULL;
    /* EDAT-1 1 MB large page. */
    if (se & Z_STE_FC) {
        uint64_t frame = se & Z_STE_LARGE_ORIGIN_MASK;
        return frame | (va & ~Z_EDAT1_PAGE_MASK);
    }

    const uint64_t *pt  = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(se));
    uint64_t pte = pt[va_px(va)];
    if (pte & Z_PTE_I) return ~0ULL;
    return (pte & PAGE_MASK) | va_bx(va);
}

bool mmu_is_large_page(const mmu_pgtbl_t *pgtbl, uint64_t va) {
    const uint64_t *r1 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);
    uint64_t e1 = r1[va_rfx(va)];
    if (e1 & Z_I_BIT) return false;
    const uint64_t *r2 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e1));
    uint64_t e2 = r2[va_rsx(va)];
    if ((e2 & Z_I_BIT) || (e2 & Z_R3E_FC)) return false;
    const uint64_t *r3 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e2));
    uint64_t e3 = r3[va_rtx(va)];
    if ((e3 & Z_I_BIT) || (e3 & Z_R3E_FC)) return false;
    const uint64_t *seg = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e3));
    uint64_t se = seg[va_sx(va)];
    return (!(se & Z_I_BIT)) && ((se & Z_STE_FC) != 0);
}

bool mmu_is_huge_page(const mmu_pgtbl_t *pgtbl, uint64_t va) {
    const uint64_t *r1 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(pgtbl->r1_phys);
    uint64_t e1 = r1[va_rfx(va)];
    if (e1 & Z_I_BIT) return false;
    const uint64_t *r2 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e1));
    uint64_t e2 = r2[va_rsx(va)];
    if (e2 & Z_I_BIT) return false;
    if (e2 & Z_R3E_FC) return true;
    const uint64_t *r3 = (const uint64_t *)(uintptr_t)hhdm_phys_to_virt(ENTRY_ORIGIN(e2));
    uint64_t e3 = r3[va_rtx(va)];
    return (!(e3 & Z_I_BIT)) && ((e3 & Z_R3E_FC) != 0);
}


void mmu_load_pgtbl(const mmu_pgtbl_t *pgtbl) {
    __asm__ volatile(
        "lctlg 1,1,%0\n"
        "ptlb\n"
        :: "Q"(pgtbl->asce) : "memory"
    );
}

void mmu_init(void) {
    uint64_t cr1;
    __asm__ volatile("stctg 1,1,%0" : "=Q"(cr1));

    spin_lock_init(&kernel_pgtbl.lock);
    kernel_pgtbl.asce    = cr1;
    kernel_pgtbl.r1_phys = cr1 & ~0xFFFULL;

    edat1_enabled = arch_cpu_has_sys_feature(ZX_SYS_FEATURE_EDAT1);
    edat2_enabled = arch_cpu_has_sys_feature(ZX_SYS_FEATURE_EDAT2);

    uint64_t *r1 = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(kernel_pgtbl.r1_phys);
    uint32_t scrubbed = 0;
    for (int i = 0; i < (int)Z_TABLE_ENTRIES; i++) {
        uint64_t e = r1[i];
        if (e & Z_I_BIT) continue;
        uint64_t r2_phys = ENTRY_ORIGIN(e);
        if (r2_phys == 0 || r2_phys >= (4ULL * 1024 * 1024 * 1024)) {
            r1[i] = Z_I_BIT | Z_TL_2048 | Z_TT_R1;
            scrubbed++;
        }
    }
    if (scrubbed)
        mmu_flush_tlb_local();

    printk("mmu: ASCE=%016llx R1=%016llx EDAT-1=%s EDAT-2=%s (scrubbed %u)\n",
           (unsigned long long)kernel_pgtbl.asce,
           (unsigned long long)kernel_pgtbl.r1_phys,
           edat1_enabled ? "on" : "off",
           edat2_enabled ? "on" : "off",
           scrubbed);
}


mmu_pgtbl_t *mmu_kernel_pgtbl(void) { return &kernel_pgtbl; }
bool mmu_has_edat1(void)             { return edat1_enabled; }
bool mmu_has_edat2(void)             { return edat2_enabled; }
