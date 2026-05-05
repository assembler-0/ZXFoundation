// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/page.h

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/zconfig.h>
#include <arch/s390x/cpu/atomic.h>


#define PF_BUDDY        (1U << 0)   ///< In buddy free list.
#define PF_HEAD         (1U << 1)   ///< Head of a compound / folio.
#define PF_TAIL         (1U << 2)   ///< Tail page of a compound.
#define PF_SLAB         (1U << 3)   ///< Owned by a kmem_cache slab.
#define PF_RESERVED     (1U << 4)   ///< Firmware / MMIO / lowcore.
#define PF_POISON       (1U << 5)   ///< Freed — not yet reallocated (UAF guard).
#define PF_PINNED       (1U << 6)   ///< Pinned for DMA or external reference.
#define PF_VMALLOC      (1U << 7)   ///< Backing a vmalloc region.
#define PF_DIRTY        (1U << 8)   ///< Page has been written since last flush.

typedef enum {
    ZONE_DMA    = 0,   ///< [0, 16 MB) — legacy DASD / channel DMA.
    ZONE_NORMAL = 1,   ///< [16 MB, end) — general kernel frames.
    ZONE_MAX    = 2,
} zone_id_t;

struct kmem_cache;
struct zx_folio;


typedef struct zx_page {
    uint32_t  flags;       ///< PF_* bitmask.
    atomic_t  refcount;    ///< Owner reference count (0 = free).
    uint8_t   order;       ///< Buddy block order (log2 of block size).
    uint8_t   zone_id;     ///< Zone membership (zone_id_t).
    uint8_t   numa_node;   ///< NUMA node (0 on single-node).
    uint8_t   _pad0;       ///< Reserved, must be 0.
    uint32_t  _pad1;       ///< Aligns union to 8-byte offset 16.

    union {
        struct kmem_cache *slab_cache; ///< PF_SLAB: owning cache.
        uint32_t compound_order;       ///< PF_HEAD: large-page order.
        uint32_t tail_offset;          ///< PF_TAIL: index delta to head.
        uint32_t buddy_next;           ///< Buddy free-list: next PFN link.
    };
    uint64_t  _pad2;       ///< Pads struct to exactly 32 bytes.
} zx_page_t;

_Static_assert(sizeof(zx_page_t) == 32, "zx_page_t must be 32 bytes");

/// @brief Overlaid on the head zx_page_t for a large page.
///        PF_HEAD is set; the following 255 tail pages have PF_TAIL.
typedef struct zx_folio {
    zx_page_t head;       ///< Must be first; inherits zone/numa/flags.
    uint32_t  nr_pages;   ///< Number of 4 KB pages in this folio (256).
    uint32_t  mapcount;   ///< Active PTEs pointing at this folio.
} zx_folio_t;

extern zx_page_t *zx_mem_map;

#define PAGE_SHIFT   12U
#define PAGE_SIZE    CONFIG_PAGE_SIZE
#define PAGE_MASK    (~(PAGE_SIZE - 1UL))

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
    uint64_t phys = hhdm_virt_to_phys((uint64_t)(uintptr_t)virt);
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
        return p - p->tail_offset;
    return p;
}
