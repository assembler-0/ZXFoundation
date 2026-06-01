// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/page.h
//
/// @brief Physical page descriptor (zx_page_t) and zone enumeration.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/zxconfig.h>
#include <zxfoundation/memory/hhdm.h>
#include <arch/s390x/cpu/atomic.h>
#include <lib/list.h>

#define PF_BUDDY        (1U << 0)   ///< In buddy free list.
#define PF_HEAD         (1U << 1)   ///< Head of a compound page.
#define PF_TAIL         (1U << 2)   ///< Tail page of a compound.
#define PF_SLAB         (1U << 3)   ///< Owned by a kmem_cache slab.
#define PF_RESERVED     (1U << 4)   ///< Firmware / MMIO / lowcore — never buddied.
#define PF_POISON       (1U << 5)   ///< Hardware-poisoned (PFMF) — in buddy free list.
#define PF_PINNED       (1U << 6)   ///< Pinned (capability table / DMA) — never reclaimed.
#define PF_VMALLOC      (1U << 7)   ///< Backing a vmalloc region.
#define PF_DIRTY        (1U << 8)   ///< Written since last flush.
#define PF_KMALLOC      (1U << 9)   ///< Large kmalloc — freed via pmm_free_pages.
#define PF_SUSPECT      (1U << 10)  ///< MCCK: corrected storage error; still usable.
#define PF_OFFLINE      (1U << 11)  ///< MCCK: uncorrected error; permanently removed.
#define PF_ZERO_READY   (1U << 12)  ///< Pre-zeroed by idle thread; ZX_GFP_ZERO skips memset.
#define PF_KEY_SET      (1U << 13)  ///< Storage key has been assigned via SSKE.

typedef enum {
    ZONE_DMA    = 0,
    ZONE_NORMAL = 1,
    ZONE_MAX    = 2,
} zone_id_t;

struct kmem_cache;

typedef struct zx_page {
    uint32_t    flags;          ///< PF_* bitmask.
    atomic_t    refcount;       ///< Owner reference count (0 = free in buddy).
    uint8_t     order;          ///< Buddy block order (log2 of block size in pages).
    uint8_t     zone_id;        ///< Zone membership (zone_id_t cast to uint8_t).
    uint8_t     numa_node;      ///< NUMA node the frame belongs to.
    uint8_t     skey;           ///< s390x storage key currently assigned.
    uint32_t    _pad0;          ///< Aligns buddy links to 8-byte boundary.

    uint64_t    buddy_next;     ///< PFN of next free block.
    uint64_t    buddy_prev;     ///< PFN of prev free block.

    union {
        struct kmem_cache *slab_cache;  ///< PF_SLAB: owning slab cache.
        uint32_t compound_order;        ///< PF_HEAD: large-page order.
        uint32_t tail_offset;           ///< PF_TAIL: index delta to head page.
        void    *mapping;               ///< Page-cache / anonymous mapping pointer.
    };

    list_head_t lru;            ///< LRU list linkage (active/inactive).

    uint64_t    _reserved;      ///< Reserved for future use.
} zx_page_t;

_Static_assert(sizeof(zx_page_t) == 64, "zx_page_t must be 64 bytes");

extern zx_page_t *zx_mem_map;

#define PAGE_SHIFT   12U
#define PAGE_SIZE    4096UL
#define PAGE_MASK    (~(PAGE_SIZE - 1UL))

#define PMM_INVALID_PFN64  (~(uint64_t)0)

static inline zx_page_t *pfn_to_page(uint64_t pfn) {
    return &zx_mem_map[pfn];
}

static inline uint64_t page_to_pfn(const zx_page_t *page) {
    return (uint64_t)(page - zx_mem_map);
}

static inline uint64_t page_to_phys(const zx_page_t *page) {
    return page_to_pfn(page) << PAGE_SHIFT;
}

static inline zx_page_t *phys_to_page(uint64_t phys) {
    return pfn_to_page(phys >> PAGE_SHIFT);
}

static inline zx_page_t *virt_to_page(const void *virt) {
    uint64_t phys = hhdm_virt_to_phys((uintptr_t)virt);
    return phys_to_page(phys);
}

static inline void *page_to_virt(const zx_page_t *page) {
    return (void *)(uintptr_t)hhdm_phys_to_virt(page_to_phys(page));
}

static inline bool page_is_head(const zx_page_t *p) {
    return (p->flags & PF_HEAD) != 0;
}

static inline zx_page_t *compound_head(zx_page_t *p) {
    if (p->flags & PF_TAIL)
        return p - (ptrdiff_t)p->tail_offset;
    return p;
}
