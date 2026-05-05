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
///        VMA INDEXING
///        ============
///        VMAs are indexed by a red-black tree keyed on vm_start for O(log n)
///        find, insert, and remove.  A vm_space_t also caches the most recently
///        touched VMA (mru_vma) to give O(1) for sequential access patterns
///        (e.g., page-fault handlers that walk adjacent VMAs).
///
///        THREAD SAFETY
///        =============
///        vm_space_t::lock (ticket spinlock + irqsave) protects the RB-tree
///        and mru_vma.  All mmu_map_page() calls happen while the lock is held
///        during insert; unmapping happens while the lock is held during remove.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/sync/spinlock.h>
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
#define LARGE_PAGE_SIZE     (1ULL << 20)             ///< 1 MB
#define LARGE_PAGE_MASK     (~(LARGE_PAGE_SIZE - 1)) ///< Mask to 1 MB boundary.
#define LARGE_PAGE_ORDER    8U                       ///< 2^8 = 256 pages = 1 MB
#define LARGE_PAGE_NR_PAGES (1U << LARGE_PAGE_ORDER) ///< 256

// ---------------------------------------------------------------------------
// vm_area_t — one virtual memory region
// ---------------------------------------------------------------------------

typedef struct vm_area {
    uint64_t    vm_start;   ///< Inclusive start, page-aligned.
    uint64_t    vm_end;     ///< Exclusive end, page-aligned.
    vm_prot_t   vm_prot;    ///< VM_READ | VM_WRITE | VM_EXEC | ...
    rb_node_t   rb_node;    ///< Intrusive RB-tree node keyed by vm_start.
} vm_area_t;

// ---------------------------------------------------------------------------
// vm_space_t — one address space
// ---------------------------------------------------------------------------

typedef struct vm_space {
    spinlock_t   lock;       ///< Protects vma_tree, vma_count, mru_vma.
    rb_root_t    vma_tree;   ///< RB-tree of vm_area_t, keyed by vm_start.
    uint64_t     vma_count;  ///< Number of VMAs currently in the tree.
    vm_area_t   *mru_vma;    ///< Most-recently-used VMA (MRU cache, 1-entry).
    uint64_t     pgtbl_root; ///< Physical address of the R1 (ASCE) table.
} vm_space_t;

// ---------------------------------------------------------------------------
// Kernel singleton
// ---------------------------------------------------------------------------

extern vm_space_t kernel_vm_space;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// @brief Initialise the VMM subsystem.
///        Must be called after pmm_init() and mmu_init(), with DAT enabled.
void vmm_init(void);

/// @brief Signal to the VMM that kmalloc is now available.
///        Transitions VMA descriptor allocation from the static early pool
///        to kmalloc().  Called by kmalloc_init() at the end of init.
void vmm_notify_slab_ready(void);

/// @brief Map a physical range into the kernel vmalloc region.
///        Useful for MMIO (ioremap) or late-discovered RAM.
///        Pages are NOT allocated from the PMM; the caller owns the physical frames.
/// @param phys  Physical start address (page-aligned).
/// @param size  Byte length (page-aligned).
/// @param prot  VM_READ | VM_WRITE | etc.
/// @return Kernel virtual address, or 0 on failure.
uint64_t vmm_map_phys(uint64_t phys, uint64_t size, vm_prot_t prot);

/// @brief Allocate a virtually-contiguous, physically-discontiguous region.
///        Individual 4 KB frames are sourced from the PMM; they are mapped
///        contiguously in the vmalloc virtual range.
/// @param size  Byte length (>= PAGE_SIZE; rounded up).
/// @param prot  Protection flags.
/// @param gfp   PMM allocation flags for the backing frames.
/// @return Kernel virtual address, or 0 on failure.
uint64_t vmm_alloc(uint64_t size, vm_prot_t prot, gfp_t gfp);

/// @brief Free a region returned by vmm_alloc().
/// @param virt  The exact address returned by vmm_alloc().
void vmm_free(uint64_t virt);

/// @brief Insert an externally-built VMA into an address space, wiring pages.
/// @param space  Target vm_space_t.
/// @param vma    VMA with vm_start, vm_end, vm_prot set.
/// @param gfp    PMM flags for backing frames.
/// @return 0 on success, -1 on failure (OOM or overlap).
int vmm_insert_vma(vm_space_t *space, vm_area_t *vma, gfp_t gfp);

/// @brief Unmap and free all backing pages of a VMA, then remove from space.
/// @param space  Address space owning the VMA.
/// @param vma    VMA to remove (must be in space->vma_tree).
void vmm_remove_vma(vm_space_t *space, vm_area_t *vma);

/// @brief Find the VMA containing 'virt', or nullptr if unmapped.
///        Checks mru_vma first for O(1) sequential access patterns.
/// @param space  Address space to search (caller must hold space->lock or
///               guarantee no concurrent modifications).
/// @param virt   Virtual address to look up.
/// @return Matching vm_area_t *, or nullptr.
vm_area_t *vmm_find_vma(vm_space_t *space, uint64_t virt);

/// @brief Print all VMAs in the space to the console (debug helper).
void vmm_dump_space(const vm_space_t *space);
