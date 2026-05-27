// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/pmm.h
//
/// @brief Physical Memory Manager (PMM) — zone-aware, NUMA-aware buddy allocator.

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

/// Maximum number of NUMA nodes the PMM tracks.
/// Matches the physical Book count on IBM Z (up to 4 books per CEC).
#define NUMA_MAX_NODES      4U

/// Sentinel for "any node" / "no node preference".
#define NUMA_NODE_ANY       ((uint8_t)0xFF)

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
/// Interrupt-context allocation: may draw from the per-zone atomic reserve.
///        Must NOT sleep or trigger reclaim.  Use only from hard-IRQ context.
#define ZX_GFP_ATOMIC       (1U << 4)

/// @brief Encode a NUMA node preference into a GFP flag word.
///        node must be in [0, NUMA_MAX_NODES-1].  Use NUMA_NODE_ANY to
///        express "caller's local node", which is the default when no
///        ZX_GFP_NODE flag is present.
#define ZX_GFP_NODE_SHIFT   8U
#define ZX_GFP_NODE_MASK    (0xFFU << ZX_GFP_NODE_SHIFT)
#define ZX_GFP_NODE(n)      (((gfp_t)(n) << ZX_GFP_NODE_SHIFT) | (1U << 5))
#define ZX_GFP_HAS_NODE(g)  ((g) & (1U << 5))
#define gfp_to_node(g)      ((uint8_t)(((g) & ZX_GFP_NODE_MASK) >> ZX_GFP_NODE_SHIFT))

/// Pages held back per zone for ZX_GFP_ATOMIC callers.
#define PMM_ATOMIC_RESERVE  64U

/// Per-CPU free-list cache: pages held per CPU to avoid zone-lock contention.
#define PCP_HIGH            16U   ///< Drain to zone when count exceeds this.
#define PCP_BATCH           8U    ///< Pages moved per refill / drain.

/// @brief Per-CPU page cache for order-0 (4 KB) allocations.
///        One instance per CPU per zone, embedded in zx_percpu_t.
///        Access is IRQ-disabled; no spinlock needed.
typedef struct pmm_pcplist {
    volatile uint32_t count;        ///< Pages currently cached.
    uint32_t zone_id;               ///< Owning zone.
    volatile uint64_t pages[PCP_HIGH + PCP_BATCH]; ///< PFN stack.
} pmm_pcplist_t;

/// @brief One entry in a per-node buddy free list within a zone.
///        Intrusive link: next PFN is stored inside zx_page_t::buddy_next.
typedef struct pmm_free_area {
    /// Head PFN of the free list (PMM_INVALID_PFN = empty).
    uint32_t head;
    /// Number of free blocks at this order.
    uint64_t count;
} pmm_free_area_t;

/// @brief Per-NUMA-node free-list set within a zone.
///        Each node maintains its own buddy free lists so allocation can
///        prefer local physical memory.
typedef struct pmm_node_area {
    pmm_free_area_t free_area[MAX_ORDER + 1]; ///< Per-order free lists.
    uint64_t        free_pages;               ///< Total 4 KB pages free on this node.
    bool            present;                  ///< True if the node has memory in this zone.
} pmm_node_area_t;

/// @brief One memory zone (DMA or NORMAL) with per-NUMA-node buddy free lists.
///        A single zone lock serialises all node-areas within the zone to
///        avoid lock-order hazards and keep the implementation tractable.
///        Lock granularity can be refined per-node in a future revision.
typedef struct pmm_zone {
    spinlock_t       lock;
    zone_id_t        id;
    uint64_t         pfn_start;
    uint64_t         pfn_end;
    uint64_t         free_pages;     ///< Total 4 KB pages free across all nodes.
    uint64_t         atomic_reserve; ///< Pages reserved for ZX_GFP_ATOMIC callers.
    pmm_node_area_t  nodes[NUMA_MAX_NODES]; ///< Per-node buddy free lists.
} pmm_zone_t;

/// @brief Flat per-zone statistics plus per-node breakdown.
typedef struct {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t reserved_pages;
    uint64_t dma_free_pages;
    uint64_t normal_free_pages;
    uint64_t node_free_pages[NUMA_MAX_NODES]; ///< Free pages per NUMA node (all zones).
} pmm_stats_t;

/// @brief Initialize the PMM from the ZXFL boot memory map.
///        Must be called exactly once, before any alloc/free.
/// @param boot  Validated pointer to the ZXFL boot protocol.
void pmm_init(const zxfl_boot_protocol_t *boot);
void pmm_verify_hhdm(const zxfl_boot_protocol_t *boot);

/// @brief Initialize per-CPU page caches for a given CPU.
///        Called once per CPU during percpu_init_bsp / percpu_init_ap.
/// @param cpu_id  Logical CPU ID (0 = BSP).
void pmm_pcplist_init(uint16_t cpu_id);

/// @brief Allocate a single 4 KB frame from the caller's local NUMA node.
/// @param gfp   Allocation flags (ZX_GFP_*). Use ZX_GFP_NODE(n) to pin a node.
/// @return Page descriptor pointer, or nullptr on failure.
zx_page_t *pmm_alloc_page(gfp_t gfp);

/// @brief Allocate a single 4 KB frame from an explicit NUMA node.
///        Falls back to other nodes if the preferred node has no free pages.
/// @param node  Preferred NUMA node (0..NUMA_MAX_NODES-1) or NUMA_NODE_ANY.
/// @param gfp   Allocation flags.
/// @return Page descriptor pointer, or nullptr on failure.
zx_page_t *pmm_alloc_page_node(uint8_t node, gfp_t gfp);

/// @brief Allocate a contiguous power-of-two block of 2^order frames
///        from the caller's local NUMA node.
/// @param order  Block size exponent (0 = 4 KB, MAX_ORDER = 4 MB).
/// @param gfp    Allocation flags.
/// @return Head page descriptor, or nullptr on failure.
zx_page_t *pmm_alloc_pages(uint32_t order, gfp_t gfp);

/// @brief Allocate a 2^order block from an explicit NUMA node.
///        Falls back to other nodes in round-robin order on exhaustion.
/// @param node   Preferred NUMA node or NUMA_NODE_ANY.
/// @param order  Block size exponent.
/// @param gfp    Allocation flags.
/// @return Head page descriptor, or nullptr on failure.
zx_page_t *pmm_alloc_pages_node(uint8_t node, uint32_t order, gfp_t gfp);

/// @brief Free a single 4 KB frame back to the buddy pool.
/// @param page  Descriptor returned by pmm_alloc_page().
void pmm_free_page(zx_page_t *page);

/// @brief Free a 2^order block back to the buddy pool.
/// @param page  Head page descriptor returned by pmm_alloc_pages().
/// @param order Must match the order used at allocation time.
void pmm_free_pages(zx_page_t *page, uint32_t order);

/// @brief Allocate contiguous frames from ZONE_DMA.
zx_page_t *pmm_dma_alloc(uint32_t order, gfp_t gfp);

/// @brief Free frames back to ZONE_DMA.
void pmm_dma_free(zx_page_t *page, uint32_t order);

/// @brief Mark a physical address range as reserved.
///        Any frames in this range that are free will be removed from the
///        buddy allocator permanently.  Idempotent.
/// @param phys_start  Inclusive start (byte address).
/// @param phys_end    Exclusive end (byte address).
void pmm_reserve_range(uint64_t phys_start, uint64_t phys_end);

/// @brief Force all CPUs to flush their per-CPU page caches back to the buddy allocator.
///        Required before pmm_reserve_range() or during critical memory pressure.
void pmm_drain_local_pcps(void);
void pmm_drain_all_pcps(void);

/// @brief Fill a pmm_stats_t snapshot.  Acquires zone locks internally.
void pmm_get_stats(pmm_stats_t *out);

/// @brief Return the highest PFN observed in the boot memory map.
///        Valid after pmm_init().  Needed by the VMM to size page tables.
uint64_t pmm_get_max_pfn(void);

/// @brief Return the zone descriptor for a given zone_id.
///        The zone lock must NOT be held by the caller.
pmm_zone_t *pmm_get_zone(zone_id_t id);

/// @brief Return the NUMA node ID for a logical CPU, or 0 if unknown.
///        Valid after pmm_init() has processed the boot protocol CPU map.
uint8_t pmm_cpu_to_node(uint16_t cpu_id);

static inline uint64_t pmm_phys_to_pfn(uint64_t phys) { return phys >> PAGE_SHIFT; }
static inline uint64_t pmm_pfn_to_phys(uint64_t pfn)  { return pfn  << PAGE_SHIFT; }

static inline uint64_t pmm_page_to_pfn(const zx_page_t *page) {
    return page_to_pfn(page);
}

static inline uint64_t pmm_page_to_phys(const zx_page_t *page) {
    return pmm_pfn_to_phys(pmm_page_to_pfn(page));
}
