// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/vmm.h
//
/// @brief Virtual Memory Manager (VMM) — kernel and user address space.
///
///        ADDRESS SPACE LAYOUT  (64-bit z/Architecture, 5-level paging)
///        ================================================================
///
///          0x0000_0000_0000_0000  ┌───────────────────────────────────────┐
///                                 │  User space  (0 .. 0x0000_8000_...)   │
///          0x0000_7FFF_FFFF_FFFF  └───────────────────────────────────────┘
///          0xFFFF_8000_0000_0000  ┌───────────────────────────────────────┐
///                                 │  HHDM  (direct map of all phys RAM)   │
///                                 │  base = CONFIG_KERNEL_VIRT_OFFSET     │
///          0xFFFF_C000_0000_0000  ├───────────────────────────────────────┤
///                                 │  vmalloc / ioremap region             │
///          0xFFFF_E000_0000_0000  ├───────────────────────────────────────┤
///                                 │  Kernel image + BSS + data            │
///          0xFFFF_FFFF_FFFF_FFFF  └───────────────────────────────────────┘
///
///        VMA TREE
///        ========
///        VMAs are indexed by an augmented RB-tree (rcu_rb_root_aug_t) keyed
///        on vm_start.  Each node carries subtree_max_end — the maximum vm_end
///        in its subtree — enabling O(log n) free-gap search for vmalloc.
///        A per-CPU hint cache (rb_pcpu_cache_t) gives O(1) on the hot
///        page-fault lookup path.
///
///        THREAD SAFETY
///        =============
///        Readers call vmm_find_vma() inside rcu_read_lock() — fully lockless.
///        Writers acquire vmm_rb_root_t::aug_root.lock (spinlock + irqsave).
///        Augmentation propagation runs under the same lock, so readers always
///        observe a tree where subtree_max_end is consistent with the pointer
///        structure they see.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/memory/vm_flags.h>
#include <lib/rbtree.h>

// ---------------------------------------------------------------------------
// vmalloc region boundaries
// ---------------------------------------------------------------------------

#define VMALLOC_START       0xFFFFC00000000000ULL
#define VMALLOC_END         0xFFFFE00000000000ULL

/// 1 MB large page geometry (EDAT-1 segment-table direct mapping).
#define LARGE_PAGE_SIZE     (1ULL << 20)
#define LARGE_PAGE_MASK     (~(LARGE_PAGE_SIZE - 1))
#define LARGE_PAGE_ORDER    8U
#define LARGE_PAGE_NR_PAGES (1U << LARGE_PAGE_ORDER)

// ---------------------------------------------------------------------------
// vm_area_t — one virtual memory region
// ---------------------------------------------------------------------------

typedef struct vm_area {
    uint64_t       vm_start;   ///< Inclusive start, page-aligned.
    uint64_t       vm_end;     ///< Exclusive end, page-aligned.
    vm_prot_t      vm_prot;    ///< VM_READ | VM_WRITE | VM_EXEC | ...
    rb_node_aug_t  rb_node;    ///< Augmented RB node; subtree_max_end maintained by tree.
    rcu_head_t     rcu;        ///< RCU callback head for deferred free.
} vm_area_t;

// ---------------------------------------------------------------------------
// vmm_rb_root_t — VMM-specific composite root (Layer 4)
//
//   Combines rcu_rb_root_aug_t (Layers 2A) with a per-CPU hint cache
//   (Layer 3).  This type is VMM-private; generic code uses the rbtree
//   layers directly.
//
//   READER (page-fault handler, vmm_find_vma):
//     rcu_read_lock();
//     vma = vmm_rb_find_vma(&root, addr);   // O(1) hint hit or O(log n)
//     rcu_read_unlock();
//
//   WRITER (vmm_insert_vma, vmm_remove_vma):
//     vmm_rb_insert_vma / vmm_rb_erase_vma acquire aug_root.lock.
//     Gap propagation runs under the same lock.
// ---------------------------------------------------------------------------

typedef struct vmm_rb_root {
    rcu_rb_root_aug_t  aug_root;        ///< RCU + augmented tree + write lock.
    rb_cache_gen_t     gen;             ///< Generation counter for cache invalidation.
    rb_pcpu_cache_t    pcpu[MAX_CPUS];  ///< Per-CPU last-found hint.
} vmm_rb_root_t;

/// @brief Augmentation callbacks for the VMA tree (defined in vmm.c).
extern const rb_aug_callbacks_t vmm_aug_callbacks;

#define VMM_RB_ROOT_INIT \
    { .aug_root = RCU_RB_ROOT_AUG_INIT(&vmm_aug_callbacks), \
      .gen      = RB_CACHE_GEN_INIT }

/// @brief Lockless RCU reader search.  Call inside rcu_read_lock().
/// @return Matching vm_area_t *, or nullptr.
vm_area_t *vmm_rb_find_vma(vmm_rb_root_t *root, uint64_t addr);

/// @brief Writer insert (acquires aug_root.lock internally).
/// @return true on success, false on overlap.
bool vmm_rb_insert_vma(vmm_rb_root_t *root, vm_area_t *vma);

/// @brief Writer erase (acquires aug_root.lock, defers free via call_rcu).
void vmm_rb_erase_vma(vmm_rb_root_t *root, vm_area_t *vma,
                      void (*free_fn)(rcu_head_t *));

// ---------------------------------------------------------------------------
// vm_space_t — one address space
// ---------------------------------------------------------------------------

typedef struct vm_space {
    vmm_rb_root_t vma_tree;   ///< Augmented RCU + per-CPU cached VMA tree.
    uint64_t      vma_count;  ///< Number of VMAs (written under aug_root.lock).
    uint64_t      pgtbl_root; ///< Physical address of the R1 (ASCE) table.
} vm_space_t;

extern vm_space_t kernel_vm_space;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// @brief Initialise the VMM subsystem.
void vmm_init(void);

/// @brief Signal that kmalloc is available; switch VMA alloc from early pool.
void vmm_notify_slab_ready(void);

/// @brief Map a physical range into the kernel vmalloc region (ioremap path).
/// @return Kernel virtual address, or 0 on failure.
uint64_t vmm_map_phys(uint64_t phys, uint64_t size, vm_prot_t prot);

/// @brief Allocate a virtually-contiguous, physically-discontiguous region.
/// @return Kernel virtual address, or 0 on failure.
uint64_t vmm_alloc(uint64_t size, vm_prot_t prot, gfp_t gfp);

/// @brief Free a region returned by vmm_alloc().
void vmm_free(uint64_t virt);

/// @brief Insert an externally-built VMA into an address space, wiring pages.
/// @return 0 on success, -1 on failure.
int vmm_insert_vma(vm_space_t *space, vm_area_t *vma, gfp_t gfp);

/// @brief Unmap and free all backing pages of a VMA, then remove from space.
void vmm_remove_vma(vm_space_t *space, vm_area_t *vma);

/// @brief Find the VMA containing virt (lockless RCU reader path).
/// @return Matching vm_area_t *, or nullptr.
vm_area_t *vmm_find_vma(vm_space_t *space, uint64_t virt);

/// @brief Print all VMAs in the space to the console (debug).
void vmm_dump_space(const vm_space_t *space);
