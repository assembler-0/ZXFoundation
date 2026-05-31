// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/pmm.h
//
/// @brief Physical Memory Manager, buddy allocator.
#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/memory/page.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <lib/list.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Maximum buddy order.  2^MAX_ORDER pages = 4 MB block.
#define MAX_ORDER           10U

/// Number of free-list entries per zone/node (one per order, plus order 0).
#define PMM_NR_ORDERS       (MAX_ORDER + 1U)

/// DMA zone boundary: 2 GB (channel subsystem 31-bit CDA constraint).
#define ZONE_DMA_LIMIT      (2048UL * 1024UL * 1024UL)

/// Maximum number of NUMA nodes the PMM tracks.
/// Matches the maximum physical Books on a z/Architecture CEC (up to 4).
#define NUMA_MAX_NODES      4U

/// Sentinel for "any node" / "no node preference".
#define NUMA_NODE_ANY       ((uint8_t)0xFF)

/// Pages held back per zone for ZX_GFP_ATOMIC callers.
#define PMM_ATOMIC_RESERVE  64U

/// Per-CPU page cache sizing.
#define PCP_HIGH            32U    ///< Hot magazine maximum depth.
#define PCP_BATCH           16U    ///< Pages moved per refill / drain.

// ---------------------------------------------------------------------------
// Watermark indices
// ---------------------------------------------------------------------------

typedef enum {
    PMM_WMARK_LOW  = 0,  ///< Trigger background reclaim when free_pages falls below this.
    PMM_WMARK_HIGH = 1,  ///< Stop reclaim when free_pages rises above this.
    PMM_NR_WMARKS  = 2,
} pmm_wmark_t;

// ---------------------------------------------------------------------------
// GFP flags
// ---------------------------------------------------------------------------

typedef uint32_t gfp_t;

#define ZX_GFP_NORMAL       (0U)           ///< Standard allocation from ZONE_NORMAL.
#define ZX_GFP_DMA          (1U << 0)      ///< Must allocate from ZONE_DMA.
#define ZX_GFP_DMA_FALLBACK (1U << 1)      ///< Try ZONE_NORMAL first, fall back to ZONE_DMA.
#define ZX_GFP_ZERO         (1U << 2)      ///< Zero-fill before returning to caller.
#define ZX_GFP_NOIRQ        (1U << 3)      ///< Caller already holds IRQs disabled; skip irqsave.
#define ZX_GFP_ATOMIC       (1U << 4)      ///< IRQ context; may draw from atomic reserve.

/// @brief Encode a NUMA node preference into a GFP flag word.
#define ZX_GFP_NODE_SHIFT   8U
#define ZX_GFP_NODE_MASK    (0xFFU << ZX_GFP_NODE_SHIFT)
#define ZX_GFP_NODE(n)      (((gfp_t)(n) << ZX_GFP_NODE_SHIFT) | (1U << 5))
#define ZX_GFP_HAS_NODE(g)  ((g) & (1U << 5))
#define gfp_to_node(g)      ((uint8_t)(((g) & ZX_GFP_NODE_MASK) >> ZX_GFP_NODE_SHIFT))

// ---------------------------------------------------------------------------
// Storage key constants
// ---------------------------------------------------------------------------

/// Storage key used for kernel-owned pages (general kernel data).
#define PMM_SKEY_KERNEL     0U
/// Storage key used for capability-table pages (hardware-enforced isolation).
#define PMM_SKEY_CAP_TABLE  1U
/// Storage key used for MCCK-suspect pages (still in service but flagged).
#define PMM_SKEY_SUSPECT    14U
/// Storage key used for poisoned / unmapped frames.
#define PMM_SKEY_POISON     15U

/// @brief One doubly-linked free-list for a single order within a NUMA node.
typedef struct pmm_free_area {
    uint64_t    head_pfn;   ///< PFN of first free block (PMM_INVALID_PFN64 = empty).
    uint64_t    tail_pfn;   ///< PFN of last free block (PMM_INVALID_PFN64 = empty).
    uint64_t    count;      ///< Number of free blocks at this order.
} pmm_free_area_t;

/// @brief Per-NUMA-node buddy allocator state within a zone.
///        Padding ensures the lock word and the bitmap reside on the first
///        cache line; the free_area array starts on the second cache line.
typedef struct __attribute__((aligned(64))) pmm_node_area {
    spinlock_t      node_lock;                      ///< Hot-path lock (node-local ops only).
    uint32_t        free_bitmap;                    ///< Bit i=1 iff free_area[i] is non-empty.
    uint64_t        free_pages;                     ///< Total 4 KB pages free on this node.
    uint64_t        suspect_pages;                  ///< Pages with PF_SUSPECT (corrected MCCK).
    uint64_t        offline_pages;                  ///< Pages with PF_OFFLINE (removed from service).
    uint64_t        watermark[PMM_NR_WMARKS];       ///< [LOW, HIGH] thresholds in 4 KB pages.
    bool            present;                        ///< True if this node has memory in this zone.
    uint8_t         node_id;
    uint8_t         _pad[6];
    list_head_t     suspect_list;                   ///< PF_SUSPECT pages (still usable).
    list_head_t     offline_list;                   ///< PF_OFFLINE pages (permanently removed).
    pmm_free_area_t free_area[PMM_NR_ORDERS];       ///< Per-order doubly-linked free lists.
} pmm_node_area_t;

/// @brief One physical memory zone (ZONE_DMA or ZONE_NORMAL).
typedef struct __attribute__((aligned(64))) pmm_zone {
    spinlock_t      lock;                           ///< Coarse: stats, cross-node fallback.
    zone_id_t       id;
    uint32_t        _pad;
    uint64_t        pfn_start;
    uint64_t        pfn_end;
    uint64_t        free_pages;                     ///< Sum across all nodes (approximate).
    uint64_t        atomic_reserve;                 ///< Pages reserved for ZX_GFP_ATOMIC.
    pmm_node_area_t nodes[NUMA_MAX_NODES];
} pmm_zone_t;

/// @brief Per-CPU order-0 page cache, one per zone.
///        Embedded in zx_percpu_t (lowcore.h).  Access is IRQ-disabled; no
///        spinlock needed because the cache is strictly per-CPU.
typedef struct __attribute__((aligned(64))) pmm_pcplist {
    uint32_t    count_hot;                          ///< Pages in the hot magazine.
    uint32_t    count_cold;                         ///< Pages in the cold magazine.
    uint8_t     zone_id;                            ///< Owning zone (zone_id_t).
    uint8_t     numa_node;                          ///< NUMA node this PCP was last refilled from.
    uint8_t     _pad[6];
    uint64_t    hot [PCP_HIGH + PCP_BATCH];         ///< Hot magazine: PFN stack (index 0 = bottom).
    uint64_t    cold[PCP_HIGH];                     ///< Cold magazine: PFN stack.
} pmm_pcplist_t;

/// @brief Flat statistics snapshot.  Filled by pmm_get_stats().
///        Stats are consistent within a zone (zone lock held per zone) but
///        not atomic across all zones simultaneously.
typedef struct {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t reserved_pages;
    uint64_t suspect_pages;
    uint64_t offline_pages;
    uint64_t dma_free_pages;
    uint64_t normal_free_pages;
    uint64_t node_free_pages[NUMA_MAX_NODES];
    uint64_t node_suspect_pages[NUMA_MAX_NODES];
} pmm_stats_t;

/// @brief Initialize the PMM from the ZXFL boot memory map.
///        Must be called exactly once, before any alloc/free.
/// @param boot  Validated pointer to the ZXFL boot protocol.
void pmm_init(const zxfl_boot_protocol_t *boot);

/// @brief Verify HHDM mapping consistency against the boot memory map.
///        Called after pmm_init(); panics on any mapping error.
/// @param boot  Same pointer passed to pmm_init().
void pmm_verify_hhdm(const zxfl_boot_protocol_t *boot);

/// @brief Initialize per-CPU page caches for one CPU.
///        Called once for the BSP, then once per AP during SMP bring-up.
/// @param cpu_id  Logical CPU ID (0 = BSP).
void pmm_pcplist_init(uint16_t cpu_id);

/// @brief Allocate a single 4 KB frame from the caller's local NUMA node.
/// @param gfp   Allocation flags.  Use ZX_GFP_NODE(n) to pin a NUMA node.
/// @return Page descriptor, or nullptr on failure (OOM or reserve exhausted).
zx_page_t *pmm_alloc_page(gfp_t gfp);

/// @brief Allocate a single 4 KB frame from an explicit NUMA node.
///        Falls back to other nodes in round-robin order on local exhaustion.
/// @param node  Preferred NUMA node (0..NUMA_MAX_NODES-1) or NUMA_NODE_ANY.
/// @param gfp   Allocation flags.
/// @return Page descriptor, or nullptr on failure.
zx_page_t *pmm_alloc_page_node(uint8_t node, gfp_t gfp);

/// @brief Allocate a contiguous 2^order block from the caller's local node.
/// @param order  Block size exponent (0 = 4 KB, MAX_ORDER = 4 MB).
/// @param gfp    Allocation flags.
/// @return Head page descriptor, or nullptr on failure.
zx_page_t *pmm_alloc_pages(uint32_t order, gfp_t gfp);

/// @brief Allocate a 2^order block from an explicit NUMA node.
/// @param node   Preferred NUMA node or NUMA_NODE_ANY.
/// @param order  Block size exponent.
/// @param gfp    Allocation flags.
/// @return Head page descriptor, or nullptr on failure.
zx_page_t *pmm_alloc_pages_node(uint8_t node, uint32_t order, gfp_t gfp);

/// @brief Allocate a pre-zeroed order-0 page.
///        Draws from the pre-zeroed pool if available; otherwise allocates
///        normally and zeros.  Use instead of pmm_alloc_page(ZX_GFP_ZERO)
///        on the critical path to avoid holding the caller's frame during
///        memset.
/// @param node  Preferred NUMA node or NUMA_NODE_ANY.
/// @param gfp   Allocation flags (ZX_GFP_ZERO is implied; do not set again).
/// @return Pre-zeroed page descriptor, or nullptr on failure.
zx_page_t *pmm_alloc_zeroed_page(uint8_t node, gfp_t gfp);

/// @brief Batch-allocate up to max_pages order-0 frames from one NUMA node.
///        All pages come from a single lock acquisition (more efficient than
///        calling pmm_alloc_page() repeatedly).
/// @param node       Preferred NUMA node or NUMA_NODE_ANY.
/// @param gfp        Allocation flags.
/// @param pages_out  Caller-supplied array of at least max_pages pointers.
/// @param max_pages  Maximum pages to allocate (must be <= PCP_BATCH * 4).
/// @return Number of pages actually allocated (0 on OOM).
uint32_t pmm_alloc_batch(uint8_t node, gfp_t gfp,
                         zx_page_t **pages_out, uint32_t max_pages);

/// @brief Free a single 4 KB frame back to its home zone/node.
///        May route through the per-CPU cache.
/// @param page  Descriptor returned by pmm_alloc_page() or pmm_alloc_page_node().
void pmm_free_page(zx_page_t *page);

/// @brief Free a 2^order block back to its home zone/node.
///        order must match the order used at allocation time.
/// @param page   Head page descriptor.
/// @param order  Must match the allocation order.  Mismatch is a kernel panic.
void pmm_free_pages(zx_page_t *page, uint32_t order);

/// @brief Batch-free count order-0 frames.
///        Drains the array into the buddy with a single lock acquisition.
/// @param pages  Array of page descriptors.
/// @param count  Number of entries in the array.
/// @return Number of pages successfully freed (count on success).
uint32_t pmm_free_batch(zx_page_t **pages, uint32_t count);

/// @brief Allocate a 2^order block from ZONE_DMA.
/// @param order  Block size exponent.
/// @param gfp    Additional GFP flags (ZX_GFP_DMA is implied).
/// @return Head page descriptor, or nullptr on failure.
zx_page_t *pmm_dma_alloc(uint32_t order, gfp_t gfp);

/// @brief Free a block back to ZONE_DMA.
/// @param page   Head page descriptor.
/// @param order  Must match the allocation order.
void pmm_dma_free(zx_page_t *page, uint32_t order);

/// @brief Mark a physical address range as reserved.
///        Removes any free frames in this range from the buddy permanently.
///        Calls pmm_drain_all_pcps() internally before modifying the buddy.
/// @param phys_start  Inclusive start (byte address, page-aligned).
/// @param phys_end    Exclusive end (byte address).
void pmm_reserve_range(uint64_t phys_start, uint64_t phys_end);

/// @brief Drain the calling CPU's per-CPU page cache to the buddy.
///        Must be called with IRQs disabled.
void pmm_drain_local_pcps(void);

/// @brief Force all CPUs to drain their PCP caches via IPI.
///        Blocks until all CPUs have completed the drain.
void pmm_drain_all_pcps(void);

/// @brief Drain the PCP of a specific CPU that is going offline.
///        Called by the CPU offline path (§9.3) before SIGP STOP.
/// @param cpu_id  Logical CPU ID of the CPU being taken offline.
void pmm_cpu_offline_drain(uint16_t cpu_id);

// ---------------------------------------------------------------------------
// MCCK recovery API
// ---------------------------------------------------------------------------

/// @brief Mark a page as suspect after a corrected storage error.
///        The page remains in service but is tracked on the suspect_list.
///        If the suspect count for the node exceeds a threshold, the page
///        is promoted to offline status automatically.
/// @param page  The page descriptor for the degraded frame.
void pmm_mark_suspect(zx_page_t *page);

/// @brief Permanently remove a page from service after an uncorrected error.
///        The page is pulled from whatever list it currently occupies (buddy,
///        PCP, or suspect_list) and placed on offline_list.
///        Returns false if the page could not be safely removed (e.g., currently
///        in use — caller must retry after reclaim).
/// @param page  The page descriptor for the failed frame.
/// @return true if the page was successfully offlined; false if it is in use.
bool pmm_offline_page(zx_page_t *page);

// ---------------------------------------------------------------------------
// Hardware storage-key API
// ---------------------------------------------------------------------------

/// @brief Assign a z/Architecture storage key to a single page.
///        Issues the SSKE instruction on every sub-page of the block.
///        Must only be called from supervisor state.
/// @param page   Page descriptor (head of block).
/// @param order  Number of 4 KB pages = 2^order.
/// @param skey   Storage key byte (bits 4:0 = ACC key, fetch/change/ref bits).
void pmm_set_page_key(zx_page_t *page, uint32_t order, uint8_t skey);

/// @brief Assign a storage key to a contiguous PFN range.
///        More efficient than calling pmm_set_page_key() per page because
///        the inner loop avoids repeated descriptor lookups.
/// @param pfn_start  First PFN in range.
/// @param count      Number of consecutive 4 KB frames.
/// @param skey       Storage key byte.
void pmm_set_key_range(uint64_t pfn_start, uint64_t count, uint8_t skey);

// ---------------------------------------------------------------------------
// Watermark query
// ---------------------------------------------------------------------------

/// @brief Query whether a zone/node is below a watermark threshold.
///        Used by the reclaim path and the slab shrinker.
/// @param zid   Zone to query.
/// @param node  NUMA node (NUMA_NODE_ANY = any node in the zone).
/// @param wm    Watermark level to check against.
/// @return true if free_pages < watermark[wm].
bool pmm_zone_below_watermark(zone_id_t zid, uint8_t node, pmm_wmark_t wm);

// ---------------------------------------------------------------------------
// Statistics and accessors
// ---------------------------------------------------------------------------

/// @brief Fill a pmm_stats_t snapshot.
///        Acquires zone locks (one at a time) internally.
void pmm_get_stats(pmm_stats_t *out);

/// @brief Return the highest PFN observed in the boot memory map.
///        Valid after pmm_init().  Used by VMM to size page tables.
uint64_t pmm_get_max_pfn(void);

/// @brief Return the zone descriptor for a given zone_id.
///        The returned pointer is valid for the kernel's lifetime.
///        The caller must not hold the zone lock before calling.
/// @param id  Zone identifier.
/// @return Pointer to the zone, or nullptr if id >= ZONE_MAX.
pmm_zone_t *pmm_get_zone(zone_id_t id);

/// @brief Return the NUMA node for a logical CPU ID, or 0 if unknown.
///        Valid after pmm_init().
/// @param cpu_id  Logical CPU ID (0 = BSP).
uint8_t pmm_cpu_to_node(uint16_t cpu_id);

// ---------------------------------------------------------------------------
// Inline helpers
// ---------------------------------------------------------------------------

static inline uint64_t pmm_phys_to_pfn(uint64_t phys) { return phys >> PAGE_SHIFT; }
static inline uint64_t pmm_pfn_to_phys(uint64_t pfn)  { return pfn  << PAGE_SHIFT; }

static inline uint64_t pmm_page_to_pfn(const zx_page_t *page) {
    return page_to_pfn(page);
}

static inline uint64_t pmm_page_to_phys(const zx_page_t *page) {
    return pmm_pfn_to_phys(pmm_page_to_pfn(page));
}
