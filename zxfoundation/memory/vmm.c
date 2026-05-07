// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/vmm.c
//
/// @brief Virtual Memory Manager — augmented RB-tree VMA management + vmalloc + THP.

#include <zxfoundation/memory/vmm.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/page.h>
#include <arch/s390x/mmu/mmu.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/zconfig.h>
#include <lib/rbtree.h>

vm_space_t kernel_vm_space;

// ---------------------------------------------------------------------------
// VMA allocator — static early pool, then kmalloc
// ---------------------------------------------------------------------------

#define EARLY_VMA_POOL_SIZE 32

static vm_area_t early_vma_pool[EARLY_VMA_POOL_SIZE];
static uint32_t  early_vma_idx  = 0;
static bool      slab_available = false;

static vm_area_t *vma_alloc(void) {
    if (slab_available)
        return (vm_area_t *)kmalloc(sizeof(vm_area_t), ZX_GFP_NORMAL);
    if (early_vma_idx >= EARLY_VMA_POOL_SIZE)
        zx_system_check(ZX_SYSCHK_MEM_OOM, "vmm: early vma pool exhausted");
    return &early_vma_pool[early_vma_idx++];
}

static void vma_rcu_free(rcu_head_t *head) {
    vm_area_t *vma = (vm_area_t *)((char *)head
                     - __builtin_offsetof(vm_area_t, rcu));
    if (slab_available)
        kfree(vma);
}


static void vma_aug_propagate(rb_node_t *n) {
    rb_node_aug_t   *aug = (rb_node_aug_t *)n;
    const vm_area_t *vma = rb_entry(n, vm_area_t, rb_node.node);

    uint64_t max_end = vma->vm_end;
    if (n->left) {
        uint64_t le = ((rb_node_aug_t *)n->left)->subtree_max_end;
        if (le > max_end) max_end = le;
    }
    if (n->right) {
        uint64_t re = ((rb_node_aug_t *)n->right)->subtree_max_end;
        if (re > max_end) max_end = re;
    }
    aug->subtree_max_end = max_end;
}

static void vma_aug_copy(rb_node_t *dst, const rb_node_t *src) {
    ((rb_node_aug_t *)dst)->subtree_max_end =
        ((const rb_node_aug_t *)src)->subtree_max_end;
}

const rb_aug_callbacks_t vmm_aug_callbacks = {
    .propagate = vma_aug_propagate,
    .copy      = vma_aug_copy,
};

// ---------------------------------------------------------------------------
// Gap-search accessors for rcu_rb_aug_find_gap()
// ---------------------------------------------------------------------------

static uint64_t vma_node_start(const rb_node_t *n) {
    return rb_entry(n, vm_area_t, rb_node.node)->vm_start;
}
static uint64_t vma_node_end(const rb_node_t *n) {
    return rb_entry(n, vm_area_t, rb_node.node)->vm_end;
}

// ---------------------------------------------------------------------------
// vmm_rb_root_t — Layer 4 operations
// ---------------------------------------------------------------------------

static int vma_addr_cmp(const rb_node_t *n, const void *arg) {
    const vm_area_t *vma  = rb_entry(n, vm_area_t, rb_node.node);
    uint64_t         addr = *(const uint64_t *)arg;
    if (addr < vma->vm_start) return  1;
    if (addr >= vma->vm_end)  return -1;
    return 0;
}

vm_area_t *vmm_rb_find_vma(vmm_rb_root_t *root, uint64_t addr) {
    // RCU-correct O(1)/O(log n) search: generation-checked hint fast path,
    // rcu_dereference miss path.  Must be called inside rcu_read_lock().
    rb_node_t *n = rcu_rb_aug_find_cached(&root->aug_root, &root->gen,
                                           root->pcpu, vma_addr_cmp, &addr);
    if (n)
        return rb_entry(n, vm_area_t, rb_node.node);
    return nullptr;
}

/// @brief Internal insert — caller must already hold aug_root.lock.
static bool vmm_rb_insert_locked(vmm_rb_root_t *root, vm_area_t *vma) {
    rcu_rb_root_aug_t *ar     = &root->aug_root;
    rb_node_t        **link   = &ar->aug.base.root;
    rb_node_t         *parent = nullptr;

    while (*link) {
        parent = *link;
        const vm_area_t *cur = rb_entry(parent, vm_area_t, rb_node.node);
        if (vma->vm_end <= cur->vm_start)
            link = &parent->left;
        else if (vma->vm_start >= cur->vm_end)
            link = &parent->right;
        else
            return false; // overlap
    }

    vma->rb_node.subtree_max_end = 0;
    rb_insert_aug(&ar->aug, &vma->rb_node.node, parent, link);
    // Bump gen and publish AFTER the structural change is complete.
    rb_cache_invalidate(&root->gen, root->pcpu);
    rcu_assign_pointer(ar->aug.base.root, ar->aug.base.root);
    return true;
}

bool vmm_rb_insert_vma(vmm_rb_root_t *root, vm_area_t *vma) {
    irqflags_t flags;
    spin_lock_irqsave(&root->aug_root.lock, &flags);
    bool ok = vmm_rb_insert_locked(root, vma);
    spin_unlock_irqrestore(&root->aug_root.lock, flags);
    return ok;
}

void vmm_rb_erase_vma(vmm_rb_root_t *root, vm_area_t *vma,
                      void (*free_fn)(rcu_head_t *)) {
    irqflags_t flags;
    spin_lock_irqsave(&root->aug_root.lock, &flags);
    rb_cache_invalidate(&root->gen, root->pcpu);
    rb_erase_aug(&root->aug_root.aug, &vma->rb_node.node);
    rcu_assign_pointer(root->aug_root.aug.base.root,
                       root->aug_root.aug.base.root);
    spin_unlock_irqrestore(&root->aug_root.lock, flags);

    call_rcu(&vma->rcu, free_fn);
}

static uint64_t vma_find_gap(vmm_rb_root_t *root,
                              uint64_t lo, uint64_t hi,
                              uint64_t size, uint64_t align) {
    return rcu_rb_aug_find_gap(&root->aug_root, size, align, lo, hi,
                               vma_node_start, vma_node_end);
}

static void vma_try_merge(vmm_rb_root_t *root, vm_area_t *vma) {
    rb_node_t *prev_n = rb_prev(&vma->rb_node.node);
    if (prev_n) {
        vm_area_t *prev = rb_entry(prev_n, vm_area_t, rb_node.node);
        if (prev->vm_end == vma->vm_start && prev->vm_prot == vma->vm_prot) {
            vma->vm_start = prev->vm_start;
            rb_erase_aug(&root->aug_root.aug, &prev->rb_node.node);
            root->aug_root.aug.cb->propagate(&vma->rb_node.node);
            rb_cache_invalidate(&root->gen, root->pcpu);
            rcu_assign_pointer(root->aug_root.aug.base.root,
                               root->aug_root.aug.base.root);
            call_rcu(&prev->rcu, vma_rcu_free);
        }
    }

    rb_node_t *next_n = rb_next(&vma->rb_node.node);
    if (next_n) {
        vm_area_t *next = rb_entry(next_n, vm_area_t, rb_node.node);
        if (vma->vm_end == next->vm_start && vma->vm_prot == next->vm_prot) {
            vma->vm_end = next->vm_end;
            rb_erase_aug(&root->aug_root.aug, &next->rb_node.node);
            root->aug_root.aug.cb->propagate(&vma->rb_node.node);
            rb_cache_invalidate(&root->gen, root->pcpu);
            rcu_assign_pointer(root->aug_root.aug.base.root,
                               root->aug_root.aug.base.root);
            call_rcu(&next->rcu, vma_rcu_free);
        }
    }
}

static bool thp_try_map_large(mmu_pgtbl_t *pgtbl, uint64_t va,
                               uint32_t prot, gfp_t gfp) {
    zx_page_t *head = pmm_alloc_pages(LARGE_PAGE_ORDER, gfp | ZX_GFP_ZERO);
    if (!head) return false;

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

static int map_small_pages(mmu_pgtbl_t *pgtbl, uint64_t va_start,
                            uint64_t va_end, uint32_t prot, gfp_t gfp) {
    for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
        zx_page_t *page = pmm_alloc_page(gfp);
        if (!page) {
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

static void unmap_and_free_range(mmu_pgtbl_t *pgtbl,
                                  uint64_t va_start, uint64_t va_end) {
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
            if (pa != ~0ULL) pmm_free_page(phys_to_page(pa));
            va += PAGE_SIZE;
        }
    }
}

void vmm_init(void) {
    kernel_vm_space.vma_tree   = (vmm_rb_root_t)VMM_RB_ROOT_INIT;
    kernel_vm_space.vma_count  = 0;
    kernel_vm_space.pgtbl_root = mmu_kernel_pgtbl()->r1_phys;

    printk("vmm: vmalloc %016llx – %016llx\n",
           (unsigned long long)VMALLOC_START,
           (unsigned long long)VMALLOC_END);
}

void vmm_notify_slab_ready(void) {
    slab_available = true;
}

vm_area_t *vmm_find_vma(vm_space_t *space, uint64_t virt) {
    rcu_read_lock();
    vm_area_t *found = vmm_rb_find_vma(&space->vma_tree, virt);
    rcu_read_unlock();
    return found;
}

int vmm_insert_vma(vm_space_t *space, vm_area_t *vma, gfp_t gfp) {
    if (!vma || vma->vm_start >= vma->vm_end ||
        (vma->vm_start & (PAGE_SIZE - 1))) {
        printk("vmm_insert_vma: invalid VMA [%llx, %llx)\n",
               (unsigned long long)(vma ? vma->vm_start : 0),
               (unsigned long long)(vma ? vma->vm_end   : 0));
        return -1;
    }

    mmu_pgtbl_t *pgtbl = mmu_kernel_pgtbl();
    uint64_t va = vma->vm_start, end = vma->vm_end;
    uint32_t thp_count = 0;

    uint64_t first_large = (va + LARGE_PAGE_SIZE - 1) & LARGE_PAGE_MASK;
    if (first_large > end) first_large = end;

    if (va < first_large &&
        map_small_pages(pgtbl, va, first_large, vma->vm_prot, gfp) != 0) {
        printk("vmm_insert_vma: small-page phase 1 failed\n");
        return -1;
    }

    uint64_t last_large = end & LARGE_PAGE_MASK;
    for (uint64_t lva = first_large; lva < last_large; lva += LARGE_PAGE_SIZE) {
        if (thp_try_map_large(pgtbl, lva, vma->vm_prot, gfp)) {
            thp_count++;
        } else if (map_small_pages(pgtbl, lva, lva + LARGE_PAGE_SIZE,
                                   vma->vm_prot, gfp) != 0) {
            unmap_and_free_range(pgtbl, vma->vm_start, lva);
            return -1;
        }
    }

    if (last_large < end && last_large >= first_large &&
        map_small_pages(pgtbl, last_large, end, vma->vm_prot, gfp) != 0) {
        unmap_and_free_range(pgtbl, vma->vm_start, last_large);
        return -1;
    }

    irqflags_t f;
    spin_lock_irqsave(&space->vma_tree.aug_root.lock, &f);
    bool ok = vmm_rb_insert_locked(&space->vma_tree, vma);
    if (ok) {
        space->vma_count++;
        vma_try_merge(&space->vma_tree, vma);
    }
    spin_unlock_irqrestore(&space->vma_tree.aug_root.lock, f);

    if (!ok) {
        printk("vmm_insert_vma: overlap [%llx, %llx)\n",
               (unsigned long long)vma->vm_start,
               (unsigned long long)vma->vm_end);
        unmap_and_free_range(pgtbl, vma->vm_start, vma->vm_end);
        return -1;
    }

    if (thp_count)
        printk("vmm: THP promoted %u × 1 MB large pages in [%016llx, %016llx)\n",
               thp_count,
               (unsigned long long)vma->vm_start,
               (unsigned long long)vma->vm_end);
    return 0;
}

void vmm_remove_vma(vm_space_t *space, vm_area_t *vma) {
    irqflags_t f;
    spin_lock_irqsave(&space->vma_tree.aug_root.lock, &f);
    space->vma_count--;
    spin_unlock_irqrestore(&space->vma_tree.aug_root.lock, f);

    if (!(vma->vm_prot & VM_IOREMAP))
        unmap_and_free_range(mmu_kernel_pgtbl(), vma->vm_start, vma->vm_end);

    vmm_rb_erase_vma(&space->vma_tree, vma, vma_rcu_free);
}

uint64_t vmm_alloc(uint64_t size, vm_prot_t prot, gfp_t gfp) {
    if (!size) return 0;
    size = (size + PAGE_SIZE - 1) & PAGE_MASK;

    vm_area_t *vma = vma_alloc();
    if (!vma) { printk("vmm_alloc: vma_alloc failed\n"); return 0; }

    uint64_t align = (size >= LARGE_PAGE_SIZE) ? LARGE_PAGE_SIZE : PAGE_SIZE;

    irqflags_t f;
    spin_lock_irqsave(&kernel_vm_space.vma_tree.aug_root.lock, &f);
    uint64_t va_start = vma_find_gap(&kernel_vm_space.vma_tree,
                                     VMALLOC_START, VMALLOC_END, size, align);
    spin_unlock_irqrestore(&kernel_vm_space.vma_tree.aug_root.lock, f);

    if (!va_start) {
        printk("vmm_alloc: no vmalloc gap for size=0x%llx\n",
               (unsigned long long)size);
        if (slab_available) kfree(vma);
        return 0;
    }

    vma->vm_start = va_start;
    vma->vm_end   = va_start + size;
    vma->vm_prot  = prot | VM_KERNEL;

    if (vmm_insert_vma(&kernel_vm_space, vma, gfp) != 0) {
        printk("vmm_alloc: vmm_insert_vma failed\n");
        if (slab_available) kfree(vma);
        return 0;
    }
    return va_start;
}

void vmm_free(uint64_t virt) {
    if (!virt) return;
    vm_area_t *vma = vmm_find_vma(&kernel_vm_space, virt);
    if (!vma) {
        printk("vmm_free: %016llx not in any VMA\n", (unsigned long long)virt);
        return;
    }
    vmm_remove_vma(&kernel_vm_space, vma);
}

uint64_t vmm_map_phys(uint64_t phys, uint64_t size, vm_prot_t prot) {
    if (!size) return 0;
    size = (size + PAGE_SIZE - 1) & PAGE_MASK;

    uint64_t align = (size >= LARGE_PAGE_SIZE) ? LARGE_PAGE_SIZE : PAGE_SIZE;

    irqflags_t f;
    spin_lock_irqsave(&kernel_vm_space.vma_tree.aug_root.lock, &f);
    uint64_t va_start = vma_find_gap(&kernel_vm_space.vma_tree,
                                     VMALLOC_START, VMALLOC_END, size, align);
    spin_unlock_irqrestore(&kernel_vm_space.vma_tree.aug_root.lock, f);

    if (!va_start) return 0;

    mmu_pgtbl_t *pgtbl = mmu_kernel_pgtbl();
    uint64_t off = 0;
    while (off < size) {
        uint64_t va = va_start + off, pa = phys + off;
        uint64_t remaining = size - off;
        if (!(va & (LARGE_PAGE_SIZE - 1)) && !(pa & (LARGE_PAGE_SIZE - 1)) &&
            remaining >= LARGE_PAGE_SIZE &&
            mmu_map_large_page(pgtbl, va, pa, prot | VM_KERNEL) == 0) {
            off += LARGE_PAGE_SIZE;
            continue;
        }
        if (mmu_map_page(pgtbl, va, pa, prot | VM_KERNEL) != 0) {
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
        spin_lock_irqsave(&kernel_vm_space.vma_tree.aug_root.lock, &f);
        vmm_rb_insert_locked(&kernel_vm_space.vma_tree, vma);
        kernel_vm_space.vma_count++;
        spin_unlock_irqrestore(&kernel_vm_space.vma_tree.aug_root.lock, f);
    }
    return va_start;
}

void vmm_dump_space(const vm_space_t *space) {
    printk("vmm: address space dump (%llu VMAs)\n",
           (unsigned long long)space->vma_count);
    rcu_read_lock();
    rb_node_t *n;
    rb_for_each(n, &((vm_space_t *)space)->vma_tree.aug_root.aug.base) {
        vm_area_t *v = rb_entry(n, vm_area_t, rb_node.node);
        printk("  [%016llx – %016llx] prot=%08x max_end=%llx\n",
               (unsigned long long)v->vm_start,
               (unsigned long long)v->vm_end,
               v->vm_prot,
               (unsigned long long)v->rb_node.subtree_max_end);
    }
    rcu_read_unlock();
}
