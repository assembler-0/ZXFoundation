// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/vmm.c
//
/// @brief Virtual Memory Manager — RB-tree VMA management + vmalloc.
///
///        VMA RB-TREE
///        ===========
///        The RB-tree is keyed by vm_area_t::vm_start.  Since vm_start
///        values are unique (VMAs may not overlap), the comparator is a
///        simple numeric comparison.
///
///        vmm_find_vma() has a one-entry MRU cache (space->mru_vma).  On a
///        cache hit, it validates that the queried address falls within
///        [mru->vm_start, mru->vm_end) and returns directly — O(1).
///        On a miss it performs the O(log n) RB-tree walk and updates mru.
///
///        VMALLOC BUMP
///        ============
///        A lock-protected cursor (vmalloc_next) advances monotonically
///        through [VMALLOC_START, VMALLOC_END).  Freed regions are not
///        reclaimed (a production extension would track free intervals
///        in a second RB-tree; the current bump is correct and safe).
///
///        EARLY BOOT
///        ==========
///        Before slab/kmalloc is up, vm_area_t descriptors are allocated
///        from a 32-entry static pool.  vmm_notify_slab_ready() is called
///        by kmalloc_init() to switch to dynamic allocation.
///
///        KOBJECT INTEGRATION
///        ===================
///        vm_space_t is intentionally NOT embedded in a kobject today.
///        The kernel has only one address space (kernel_vm_space) which
///        lives forever.  When user processes are added, vm_space_t will
///        gain a kobject header for reference-counted lifetime management.

#include <zxfoundation/memory/vmm.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/page.h>
#include <arch/s390x/mmu.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/spinlock.h>
#include <zxfoundation/zconfig.h>
#include <lib/rbtree.h>

// ---------------------------------------------------------------------------
// Kernel address space singleton
// ---------------------------------------------------------------------------

vm_space_t kernel_vm_space;

// ---------------------------------------------------------------------------
// vmalloc bump cursor
// ---------------------------------------------------------------------------

static uint64_t   vmalloc_next = VMALLOC_START;
static spinlock_t vmalloc_lock = SPINLOCK_INIT;

// ---------------------------------------------------------------------------
// Early VMA pool (pre-slab bootstrap)
// ---------------------------------------------------------------------------

#define EARLY_VMA_POOL_SIZE 32

static vm_area_t  early_vma_pool[EARLY_VMA_POOL_SIZE];
static uint32_t   early_vma_idx  = 0;
static bool       slab_available = false;

static vm_area_t *vma_alloc(void) {
    if (slab_available)
        return (vm_area_t *)kmalloc(sizeof(vm_area_t));
    if (early_vma_idx >= EARLY_VMA_POOL_SIZE)
        panic("vmm: early VMA pool exhausted");
    vm_area_t *v = &early_vma_pool[early_vma_idx++];
    return v;
}

static void vma_free(vm_area_t *vma) {
    if (!slab_available) return; // early-pool VMAs are never freed individually
    kfree(vma);
}

// ---------------------------------------------------------------------------
// RB-tree helpers: insert / find / remove
// ---------------------------------------------------------------------------

/// @brief Insert vma into space->vma_tree.
///        Caller must hold space->lock.
///        Returns false if the range overlaps an existing VMA.
static bool vma_rb_insert(vm_space_t *space, vm_area_t *vma) {
    rb_node_t **link = &space->vma_tree.root;
    rb_node_t  *parent = nullptr;

    while (*link) {
        parent = *link;
        vm_area_t *cur = rb_entry(parent, vm_area_t, rb_node);

        if (vma->vm_end <= cur->vm_start) {
            link = &parent->left;
        } else if (vma->vm_start >= cur->vm_end) {
            link = &parent->right;
        } else {
            // Overlap — reject.
            return false;
        }
    }

    rb_link_node(&vma->rb_node, parent, link);
    rb_insert_fixup(&space->vma_tree, &vma->rb_node);
    space->vma_count++;
    return true;
}

/// @brief Find the VMA whose range contains 'addr'.
///        Caller must hold space->lock or guarantee exclusive access.
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

/// @brief Remove vma from space->vma_tree.
///        Caller must hold space->lock.
static void vma_rb_remove(vm_space_t *space, vm_area_t *vma) {
    rb_erase(&space->vma_tree, &vma->rb_node);
    space->vma_count--;
    // Invalidate MRU if it pointed at the removed VMA.
    if (space->mru_vma == vma)
        space->mru_vma = nullptr;
}

// ---------------------------------------------------------------------------
// vmm_init
// ---------------------------------------------------------------------------

void vmm_init(void) {
    spin_lock_init(&kernel_vm_space.lock);
    kernel_vm_space.vma_tree   = (rb_root_t)RB_ROOT_INIT;
    kernel_vm_space.vma_count  = 0;
    kernel_vm_space.mru_vma    = nullptr;
    kernel_vm_space.pgtbl_root = mmu_kernel_pgtbl()->r1_phys;

    printk("vmm: kernel space ready (vmalloc %016llx – %016llx)\n",
           (unsigned long long)VMALLOC_START,
           (unsigned long long)VMALLOC_END);
}

void vmm_notify_slab_ready(void) {
    slab_available = true;
}

// ---------------------------------------------------------------------------
// vmm_find_vma
// ---------------------------------------------------------------------------

vm_area_t *vmm_find_vma(vm_space_t *space, uint64_t virt) {
    // Fast O(1) MRU check — covers sequential page-fault patterns.
    vm_area_t *mru = space->mru_vma;
    if (mru && virt >= mru->vm_start && virt < mru->vm_end)
        return mru;

    // O(log n) RB-tree walk.
    vm_area_t *found = vma_rb_find(space, virt);
    if (found)
        space->mru_vma = found; // update MRU
    return found;
}

// ---------------------------------------------------------------------------
// vmm_insert_vma
// ---------------------------------------------------------------------------

int vmm_insert_vma(vm_space_t *space, vm_area_t *vma, gfp_t gfp) {
    if (!vma || vma->vm_start >= vma->vm_end) return -1;
    if (vma->vm_start & (PAGE_SIZE - 1))       return -1;

    mmu_pgtbl_t *pgtbl = mmu_kernel_pgtbl();

    // Map backing pages before acquiring the space lock to minimise lock hold time.
    for (uint64_t va = vma->vm_start; va < vma->vm_end; va += PAGE_SIZE) {
        zx_page_t *page = pmm_alloc_page(gfp);
        if (!page) {
            // Roll back already-mapped pages.
            for (uint64_t rv = vma->vm_start; rv < va; rv += PAGE_SIZE) {
                uint64_t pa = mmu_virt_to_phys(pgtbl, rv);
                mmu_unmap_page(pgtbl, rv);
                if (pa != ~0ULL) pmm_free_page(phys_to_page(pa));
            }
            return -1;
        }
        if (mmu_map_page(pgtbl, va, pmm_page_to_phys(page), vma->vm_prot) != 0) {
            pmm_free_page(page);
            for (uint64_t rv = vma->vm_start; rv < va; rv += PAGE_SIZE) {
                uint64_t pa = mmu_virt_to_phys(pgtbl, rv);
                mmu_unmap_page(pgtbl, rv);
                if (pa != ~0ULL) pmm_free_page(phys_to_page(pa));
            }
            return -1;
        }
    }

    irqflags_t f;
    spin_lock_irqsave(&space->lock, &f);
    bool ok = vma_rb_insert(space, vma);
    spin_unlock_irqrestore(&space->lock, f);

    if (!ok) {
        // Overlap detected — undo the page mappings we just made.
        for (uint64_t va = vma->vm_start; va < vma->vm_end; va += PAGE_SIZE) {
            uint64_t pa = mmu_virt_to_phys(pgtbl, va);
            mmu_unmap_page(pgtbl, va);
            if (pa != ~0ULL) pmm_free_page(phys_to_page(pa));
        }
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// vmm_remove_vma
// ---------------------------------------------------------------------------

void vmm_remove_vma(vm_space_t *space, vm_area_t *vma) {
    irqflags_t f;
    spin_lock_irqsave(&space->lock, &f);
    vma_rb_remove(space, vma);
    spin_unlock_irqrestore(&space->lock, f);

    // Unmap and free backing pages (IOREMAP VMAs don't own their pages).
    if (!(vma->vm_prot & VM_IOREMAP)) {
        mmu_pgtbl_t *pgtbl = mmu_kernel_pgtbl();
        for (uint64_t va = vma->vm_start; va < vma->vm_end; va += PAGE_SIZE) {
            uint64_t pa = mmu_virt_to_phys(pgtbl, va);
            mmu_unmap_page(pgtbl, va);
            if (pa != ~0ULL) pmm_free_page(phys_to_page(pa));
        }
    }
    vma_free(vma);
}

// ---------------------------------------------------------------------------
// vmm_alloc — virtually-contiguous, physically-discontiguous
// ---------------------------------------------------------------------------

uint64_t vmm_alloc(uint64_t size, vm_prot_t prot, gfp_t gfp) {
    if (!size) return 0;
    size = (size + PAGE_SIZE - 1) & PAGE_MASK;

    vm_area_t *vma = vma_alloc();
    if (!vma) return 0;

    irqflags_t f;
    spin_lock_irqsave(&vmalloc_lock, &f);
    if (vmalloc_next + size > VMALLOC_END || vmalloc_next + size < vmalloc_next) {
        spin_unlock_irqrestore(&vmalloc_lock, f);
        vma_free(vma);
        return 0;
    }
    uint64_t va_start = vmalloc_next;
    vmalloc_next += size;
    spin_unlock_irqrestore(&vmalloc_lock, f);

    vma->vm_start = va_start;
    vma->vm_end   = va_start + size;
    vma->vm_prot  = prot | VM_KERNEL;

    if (vmm_insert_vma(&kernel_vm_space, vma, gfp) != 0) {
        vma_free(vma);
        return 0;
    }
    return va_start;
}

// ---------------------------------------------------------------------------
// vmm_free
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// vmm_map_phys — ioremap-style mapping of an existing physical range
// ---------------------------------------------------------------------------

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
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        if (mmu_map_page(pgtbl, va_start + off, phys + off, prot | VM_KERNEL) != 0) {
            for (uint64_t rv = 0; rv < off; rv += PAGE_SIZE)
                mmu_unmap_page(pgtbl, va_start + rv);
            return 0;
        }
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

// ---------------------------------------------------------------------------
// vmm_dump_space — diagnostic
// ---------------------------------------------------------------------------

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
