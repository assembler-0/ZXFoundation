// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/pmm.h
//
/// @brief Physical Memory Manager (PMM) — zone-aware buddy allocator.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/memory/page.h>
#include <arch/s390x/init/zxfl/zxfl.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define PMM_INVALID_PFN     (~(uint32_t)0)

/// Maximum buddy order.  2^MAX_ORDER = 1024 pages = 4 MB.
#define MAX_ORDER           10U

/// Number of free-list entries per zone (one per order level).
#define MAX_ORDER_NR_PAGES  (1U << MAX_ORDER)

/// DMA zone boundary: 16 MB.
#define ZONE_DMA_LIMIT      (16UL * 1024 * 1024)

typedef uint32_t gfp_t;

/// Allocate from ZONE_NORMAL (default).
#define ZX_GFP_NORMAL       (0U)
/// Require allocation from ZONE_DMA (below 16 MB).
#define ZX_GFP_DMA          (1U << 0)
/// Allow falling back to ZONE_DMA if NORMAL is exhausted.
#define ZX_GFP_DMA_FALLBACK (1U << 1)
/// Zero the returned page before handing it to the caller.
#define ZX_GFP_ZERO         (1U << 2)
/// Caller already holds a lock with IRQs disabled; skip irqsave.
#define ZX_GFP_NOIRQ        (1U << 3)

/// @brief One entry in a zone's per-order free list.
///        The list is intrusive: next/prev are PFNs, not pointers, so that
///        the list can survive across HHDM remap without pointer fixups.
typedef struct pmm_free_area {
    /// Head PFN of the free list (PMM_INVALID_PFN = empty).
    uint32_t head;
    /// Number of free blocks at this order.
    uint64_t count;
} pmm_free_area_t;

/// @brief One memory zone with its own buddy free lists and lock.
typedef struct pmm_zone {
    spinlock_t       lock;
    zone_id_t        id;
    uint64_t         pfn_start;
    uint64_t         pfn_end;
    uint64_t         free_pages;    ///< Total 4 KB pages currently free.
    pmm_free_area_t  free_area[MAX_ORDER + 1];
} pmm_zone_t;

typedef struct {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t reserved_pages;
    uint64_t dma_free_pages;
    uint64_t normal_free_pages;
} pmm_stats_t;

/// @brief Initialize the PMM from the ZXFL boot memory map.
///        Must be called exactly once, before any alloc/free.
/// @param boot  Validated pointer to the ZXFL boot protocol.
void pmm_init(const zxfl_boot_protocol_t *boot);

/// @brief Allocate a single 4 KB frame.
/// @param gfp   Allocation flags (ZX_GFP_*).
/// @return Page descriptor pointer, or nullptr on failure.
zx_page_t *pmm_alloc_page(gfp_t gfp);

/// @brief Allocate a contiguous power-of-two block of 2^order frames.
/// @param order  Block size exponent (0 = 4 KB, MAX_ORDER = 4 MB).
/// @param gfp    Allocation flags.
/// @return Head page descriptor, or nullptr on failure.
zx_page_t *pmm_alloc_pages(uint32_t order, gfp_t gfp);

/// @brief Free a single 4 KB frame back to the buddy pool.
/// @param page  Descriptor returned by pmm_alloc_page().
void pmm_free_page(zx_page_t *page);

/// @brief Free a 2^order block back to the buddy pool.
/// @param page  Head page descriptor returned by pmm_alloc_pages().
/// @param order Must match the order used at allocation time.
void pmm_free_pages(zx_page_t *page, uint32_t order);

/// @brief Mark a physical address range as reserved.
///        Any frames in this range that are free will be removed from the
///        buddy allocator permanently.  Idempotent.
/// @param phys_start  Inclusive start (byte address).
/// @param phys_end    Exclusive end (byte address).
void pmm_reserve_range(uint64_t phys_start, uint64_t phys_end);

/// @brief Fill a pmm_stats_t snapshot.  Acquires zone locks internally.
void pmm_get_stats(pmm_stats_t *out);

/// @brief Return the highest PFN observed in the boot memory map.
///        Valid after pmm_init().  Needed by the VMM to size page tables.
uint64_t pmm_get_max_pfn(void);

/// @brief Return the zone descriptor for a given zone_id.
///        The zone lock must NOT be held by the caller.
pmm_zone_t *pmm_get_zone(zone_id_t id);

static inline uint64_t pmm_phys_to_pfn(uint64_t phys) { return phys >> PAGE_SHIFT; }
static inline uint64_t pmm_pfn_to_phys(uint64_t pfn)  { return pfn  << PAGE_SHIFT; }

static inline uint64_t pmm_page_to_pfn(const zx_page_t *page) {
    return page_to_pfn(page);
}

static inline uint64_t pmm_page_to_phys(const zx_page_t *page) {
    return pmm_pfn_to_phys(pmm_page_to_pfn(page));
}
