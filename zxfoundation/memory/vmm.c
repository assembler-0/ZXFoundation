// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/vmm.c
//
/// @brief Virtual Memory Manager — RB-tree VMA management + vmalloc + THP.

#include <zxfoundation/memory/vmm.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/page.h>
#include <arch/s390x/mmu/mmu.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/spinlock.h>
#include <zxfoundation/zconfig.h>
#include <lib/rbtree.h>

vm_space_t kernel_vm_space;

static uint64_t   vmalloc_next = VMALLOC_START;
static spinlock_t vmalloc_lock = SPINLOCK_INIT;

#define EARLY_VMA_POOL_SIZE 32

static vm_area_t  early_vma_pool[EARLY_VMA_POOL_SIZE];
static uint32_t   early_vma_idx  = 0;
static bool       slab_available = false;

static vm_area_t *vma_alloc(void) {
    if (slab_available)
        return (vm_area_t *)kmalloc(sizeof(vm_area_t));
    if (early_vma_idx >= EARLY_VMA_POOL_SIZE)
        panic("vmm: early vma pool exhausted");
    vm_area_t *v = &early_vma_pool[early_vma_idx++];
    return v;
}

static void vma_free(vm_area_t *vma) {
    if (!slab_available) return;
    kfree(vma);
}

static bool vma_rb_insert(vm_space_t *space, vm_area_t *vma) {
    rb_node_t **link = &space->vma_tree.root;
    rb_node_t  *parent = nullptr;

    while (*link) {
        parent = *link;
        vm_area_t *cur = rb_entry(parent, vm_area_t, rb_node);

        if (vma->vm_end <= cur->vm_start)
            link = &parent->left;
        else if (vma->vm_start >= cur->vm_end)
            link = &parent->right;
        else
            return false; // overlap
    }

    rb_link_node(&vma->rb_node, parent, link);
    rb_insert_fixup(&space->vma_tree, &vma->rb_node);
    space->vma_count++;
    return true;
}

static vm_area_t *vma_rb_find(const vm_space_t *space, uint64_t addr) {
    const rb_node_t *n = space->vma_tree.root;
    while (n) {
        vm_area_t *cur = rb_entry(n, vm_area_t, rb_node);
        if (addr < cur->vm_start)
            n = n->left;
        else if (addr >= cur->vm_end)
            n = n->right;
        else
            return cur;
    }
    return nullptr;
}

static void vma_rb_remove(vm_space_t *space, vm_area_t *vma) {
    rb_erase(&space->vma_tree, &vma->rb_node);
    space->vma_count--;
    if (space->mru_vma == vma)
        space->mru_vma = nullptr;
}

/// @brief Attempt to map a 1 MB-aligned VA chunk as a single large page.
///        Allocates order-8 (256 contiguous 4 KB pages) from the PMM.
///        If successful, installs an EDAT-1 FC=1 STE via mmu_map_large_page.
/// @return true if the large page was successfully mapped, false on fallback.
static bool thp_try_map_large(mmu_pgtbl_t *pgtbl, uint64_t va,
                              uint32_t prot, gfp_t gfp) {
    zx_page_t *head = pmm_alloc_pages(LARGE_PAGE_ORDER, gfp | ZX_GFP_ZERO);
    if (!head)
        return false;  // fragmented — fall back to 4 KB

    uint64_t pa = pmm_page_to_phys(head);

    head->flags |= PF_HEAD;
    head->compound_order = LARGE_PAGE_ORDER;
    for (uint32_t i = 1; i < LARGE_PAGE_NR_PAGES; i++) {
        zx_page_t *tail = pfn_to_page(page_to_pfn(head) + i);
        tail->flags |= PF_TAIL;
        tail->tail_offset = i;
    }

    if (mmu_map_large_page(pgtbl, va, pa, prot) != 0) {
        head->flags &= ~PF_HEAD;
        for (uint32_t i = 1; i < LARGE_PAGE_NR_PAGES; i++) {
            zx_page_t *tail = pfn_to_page(page_to_pfn(head) + i);
            tail->flags &= ~PF_TAIL;
        }
        pmm_free_pages(head, LARGE_PAGE_ORDER);
        return false;
    }

    return true;
}

/// @brief Map a range of 4 KB pages (fallback path or non-aligned region).
/// @return 0 on success, -1 on failure (pages already mapped are rolled back).
static int map_small_pages(mmu_pgtbl_t *pgtbl, uint64_t va_start,
                           uint64_t va_end, uint32_t prot, gfp_t gfp) {
    for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
        zx_page_t *page = pmm_alloc_page(gfp);
        if (!page) {
            // Roll back.
            for (uint64_t rv = va_start; rv < va; rv += PAGE_SIZE) {
                uint64_t pa = mmu_virt_to_phys(pgtbl, rv);
                mmu_unmap_page(pgtbl, rv);
                if (pa != ~0ULL) pmm_free_page(phys_to_page(pa));
            }
            return -1;
        }
        if (mmu_map_page(pgtbl, va, pmm_page_to_phys(page), prot) != 0) {
            pmm_free_page(page);
            for (uint64_t rv = va_start; rv < va; rv += PAGE_SIZE) {
                uint64_t pa = mmu_virt_to_phys(pgtbl, rv);
                mmu_unmap_page(pgtbl, rv);
                if (pa != ~0ULL) pmm_free_page(phys_to_page(pa));
            }
            return -1;
        }
    }
    return 0;
}

/// @brief Unmap and free all backing pages in [va_start, va_end).
///        Automatically detects 1 MB large pages and frees compound blocks.
static void unmap_and_free_range(mmu_pgtbl_t *pgtbl, uint64_t va_start,
                                 uint64_t va_end) {
    uint64_t va = va_start;
    while (va < va_end) {
        if (!(va & (LARGE_PAGE_SIZE - 1)) &&
            (va + LARGE_PAGE_SIZE <= va_end) &&
            mmu_is_large_page(pgtbl, va)) {

            uint64_t pa = mmu_virt_to_phys(pgtbl, va);
            mmu_unmap_page(pgtbl, va);
            if (pa != ~0ULL) {
                zx_page_t *head = phys_to_page(pa & LARGE_PAGE_MASK);
                head->flags &= ~PF_HEAD;
                for (uint32_t i = 1; i < LARGE_PAGE_NR_PAGES; i++) {
                    zx_page_t *tail = pfn_to_page(page_to_pfn(head) + i);
                    tail->flags &= ~PF_TAIL;
                }
                pmm_free_pages(head, LARGE_PAGE_ORDER);
            }
            va += LARGE_PAGE_SIZE;
        } else {
            uint64_t pa = mmu_virt_to_phys(pgtbl, va);
            mmu_unmap_page(pgtbl, va);
            if (pa != ~0ULL)
                pmm_free_page(phys_to_page(pa));
            va += PAGE_SIZE;
        }
    }
}


void vmm_init(void) {
    spin_lock_init(&kernel_vm_space.lock);
    kernel_vm_space.vma_tree   = (rb_root_t)RB_ROOT_INIT;
    kernel_vm_space.vma_count  = 0;
    kernel_vm_space.mru_vma    = nullptr;
    kernel_vm_space.pgtbl_root = mmu_kernel_pgtbl()->r1_phys;

    printk("vmm: kernel space ready (vmalloc %016llx – %016llx, THP enabled)\n",
           (unsigned long long)VMALLOC_START,
           (unsigned long long)VMALLOC_END);
}

void vmm_notify_slab_ready(void) {
    slab_available = true;
}

vm_area_t *vmm_find_vma(vm_space_t *space, uint64_t virt) {
    vm_area_t *mru = space->mru_vma;
    if (mru && virt >= mru->vm_start && virt < mru->vm_end)
        return mru;

    vm_area_t *found = vma_rb_find(space, virt);
    if (found)
        space->mru_vma = found;
    return found;
}

int vmm_insert_vma(vm_space_t *space, vm_area_t *vma, gfp_t gfp) {
    if (!vma) {
        printk("vmm_insert_vma: NULL VMA\n");
        return -1;
    }
    if (vma->vm_start >= vma->vm_end) {
        printk("vmm_insert_vma: invalid range [%llx, %llx)\n",
               (unsigned long long)vma->vm_start, (unsigned long long)vma->vm_end);
        return -1;
    }
    if (vma->vm_start & (PAGE_SIZE - 1)) {
        printk("vmm_insert_vma: unaligned start %llx\n",
               (unsigned long long)vma->vm_start);
        return -1;
    }

    mmu_pgtbl_t *pgtbl = mmu_kernel_pgtbl();
    uint64_t va = vma->vm_start;
    uint64_t end = vma->vm_end;
    uint32_t thp_count = 0;

    uint64_t first_large = (va + LARGE_PAGE_SIZE - 1) & LARGE_PAGE_MASK;
    if (first_large > end) first_large = end;

    if (va < first_large) {
        if (map_small_pages(pgtbl, va, first_large, vma->vm_prot, gfp) != 0) {
            printk("vmm_insert_vma: Phase 1 failure [%llx, %llx)\n",
                   (unsigned long long)va, (unsigned long long)first_large);
            return -1;
        }
    }

    uint64_t last_large = end & LARGE_PAGE_MASK;
    for (uint64_t lva = first_large; lva < last_large; lva += LARGE_PAGE_SIZE) {
        if (thp_try_map_large(pgtbl, lva, vma->vm_prot, gfp)) {
            thp_count++;
        } else {
            if (map_small_pages(pgtbl, lva, lva + LARGE_PAGE_SIZE,
                                vma->vm_prot, gfp) != 0) {
                printk("vmm_insert_vma: Phase 2 failure at %llx\n", (unsigned long long)lva);
                unmap_and_free_range(pgtbl, vma->vm_start, lva);
                return -1;
            }
        }
    }

    if (last_large < end && last_large >= first_large) {
        if (map_small_pages(pgtbl, last_large, end, vma->vm_prot, gfp) != 0) {
            printk("vmm_insert_vma: Phase 3 failure [%llx, %llx)\n",
                   (unsigned long long)last_large, (unsigned long long)end);
            unmap_and_free_range(pgtbl, vma->vm_start, last_large);
            return -1;
        }
    }

    irqflags_t f;
    spin_lock_irqsave(&space->lock, &f);
    bool ok = vma_rb_insert(space, vma);
    spin_unlock_irqrestore(&space->lock, f);

    if (!ok) {
        printk("vmm_insert_vma: Phase 4 failure (overlap) [%llx, %llx)\n",
               (unsigned long long)vma->vm_start, (unsigned long long)vma->vm_end);
        unmap_and_free_range(pgtbl, vma->vm_start, vma->vm_end);
        return -1;
    }

    if (thp_count > 0) {
        printk("vmm: THP promoted %u × 1 MB large pages in [%016llx, %016llx)\n",
               thp_count,
               (unsigned long long)vma->vm_start,
               (unsigned long long)vma->vm_end);
    }

    return 0;
}

void vmm_remove_vma(vm_space_t *space, vm_area_t *vma) {
    irqflags_t f;
    spin_lock_irqsave(&space->lock, &f);
    vma_rb_remove(space, vma);
    spin_unlock_irqrestore(&space->lock, f);

    if (!(vma->vm_prot & VM_IOREMAP)) {
        mmu_pgtbl_t *pgtbl = mmu_kernel_pgtbl();
        unmap_and_free_range(pgtbl, vma->vm_start, vma->vm_end);
    }
    vma_free(vma);
}

uint64_t vmm_alloc(uint64_t size, vm_prot_t prot, gfp_t gfp) {
    if (!size) return 0;
    size = (size + PAGE_SIZE - 1) & PAGE_MASK;

    vm_area_t *vma = vma_alloc();
    if (!vma) {
        printk("vmm_alloc: vma_alloc failed\n");
        return 0;
    }

    // Align bump cursor to 1 MB for maximum THP eligibility.
    irqflags_t f;
    spin_lock_irqsave(&vmalloc_lock, &f);
    uint64_t aligned = (vmalloc_next + LARGE_PAGE_SIZE - 1) & LARGE_PAGE_MASK;
    // For small allocations (< 1MB), don't waste a full 1MB gap.
    if (size < LARGE_PAGE_SIZE)
        aligned = vmalloc_next;
    if (aligned + size > VMALLOC_END || aligned + size < aligned) {
        printk("vmm_alloc: out of vmalloc space (next=0x%llx, size=0x%llx)\n",
               (unsigned long long)vmalloc_next, (unsigned long long)size);
        spin_unlock_irqrestore(&vmalloc_lock, f);
        vma_free(vma);
        return 0;
    }
    uint64_t va_start = aligned;
    vmalloc_next = aligned + size;
    spin_unlock_irqrestore(&vmalloc_lock, f);

    vma->vm_start = va_start;
    vma->vm_end   = va_start + size;
    vma->vm_prot  = prot | VM_KERNEL;

    if (vmm_insert_vma(&kernel_vm_space, vma, gfp) != 0) {
        printk("vmm_alloc: vmm_insert_vma failed\n");
        vma_free(vma);
        return 0;
    }
    return va_start;
}

void vmm_free(uint64_t virt) {
    if (!virt) return;
    irqflags_t f;
    spin_lock_irqsave(&kernel_vm_space.lock, &f);
    vm_area_t *vma = vmm_find_vma(&kernel_vm_space, virt);
    spin_unlock_irqrestore(&kernel_vm_space.lock, f);
    if (!vma) {
        printk("vmm_free: %016llx not in any VMA\n", (unsigned long long)virt);
        return;
    }
    vmm_remove_vma(&kernel_vm_space, vma);
}

uint64_t vmm_map_phys(uint64_t phys, uint64_t size, vm_prot_t prot) {
    if (!size) return 0;
    size = (size + PAGE_SIZE - 1) & PAGE_MASK;

    irqflags_t f;
    spin_lock_irqsave(&vmalloc_lock, &f);
    if (vmalloc_next + size > VMALLOC_END) {
        spin_unlock_irqrestore(&vmalloc_lock, f);
        return 0;
    }
    uint64_t va_start = vmalloc_next;
    vmalloc_next += size;
    spin_unlock_irqrestore(&vmalloc_lock, f);

    mmu_pgtbl_t *pgtbl = mmu_kernel_pgtbl();

    uint64_t off = 0;
    while (off < size) {
        uint64_t va = va_start + off;
        uint64_t pa = phys + off;
        uint64_t remaining = size - off;

        if (!(va & (LARGE_PAGE_SIZE - 1)) &&
            !(pa & (LARGE_PAGE_SIZE - 1)) &&
            remaining >= LARGE_PAGE_SIZE) {
            if (mmu_map_large_page(pgtbl, va, pa, prot | VM_KERNEL) == 0) {
                off += LARGE_PAGE_SIZE;
                continue;
            }
        }
        if (mmu_map_page(pgtbl, va, pa, prot | VM_KERNEL) != 0) {
            // Roll back.
            for (uint64_t rv = 0; rv < off; rv += PAGE_SIZE)
                mmu_unmap_page(pgtbl, va_start + rv);
            return 0;
        }
        off += PAGE_SIZE;
    }

    vm_area_t *vma = vma_alloc();
    if (vma) {
        vma->vm_start = va_start;
        vma->vm_end   = va_start + size;
        vma->vm_prot  = prot | VM_KERNEL | VM_IOREMAP;
        spin_lock_irqsave(&kernel_vm_space.lock, &f);
        vma_rb_insert(&kernel_vm_space, vma);
        spin_unlock_irqrestore(&kernel_vm_space.lock, f);
    }
    return va_start;
}

void vmm_dump_space(const vm_space_t *space) {
    irqflags_t f;
    spin_lock_irqsave((spinlock_t *)&space->lock, &f);
    printk("vmm: address space dump (%llu VMAs)\n",
           (unsigned long long)space->vma_count);
    rb_node_t *n;
    rb_for_each(n, (rb_root_t *)&space->vma_tree) {
        vm_area_t *v = rb_entry(n, vm_area_t, rb_node);
        printk("  [%016llx – %016llx] prot=%08x\n",
               (unsigned long long)v->vm_start,
               (unsigned long long)v->vm_end,
               v->vm_prot);
    }
    spin_unlock_irqrestore((spinlock_t *)&space->lock, f);
}
