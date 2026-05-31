// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/pmm.c
//
/// @brief buddy physical memory manager.

#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zxconfig.h>
#include <zxfoundation/percpu.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/mmu/mmu.h>
#include <arch/s390x/cpu/ipi.h>
#include <arch/s390x/linksym.h>
#include <lib/string.h>

/// Two physical memory zones.
static pmm_zone_t zones[ZONE_MAX];

/// Flat array of page descriptors; allocated from physical memory during init.
/// Never freed.  Exported so pfn_to_page() can be inlined.
zx_page_t *zx_mem_map = nullptr;

/// Highest PFN seen in the boot memory map.
static uint64_t max_pfn_global = 0;

/// True once pmm_init() completes successfully.
static bool pmm_ready = false;

/// CPU-to-NUMA-node mapping.  Indexed by logical CPU ID.
/// Default 0 is intentionally safe: unknown CPUs fall back to node 0.
static uint8_t cpu_to_node_table[MAX_CPUS];

/// @brief Issue PFMF to zero a 4 KB frame and set its storage key.
///        On entry the frame must not be mapped with a conflicting key.
///        Called with the frame already removed from the buddy list.
/// @param phys  Physical address of the 4 KB frame (must be 4 KB aligned).
/// @param skey  Storage key to assign after zeroing.
static inline void pmm_pfmf_zero_and_key(uint64_t phys, uint8_t skey) {
    if (arch_cpu_has_sys_feature(ZX_SYS_FEATURE_PFMF)) {
        uint64_t op1 = ((uint64_t)(skey & 0x0FU) << 4) | (1U << 11) | (1U << 10);
        arch_pfmf(op1, phys);
    } else {
        // Software fallback: memset then SSKE.
        void *virt = (void *)(uintptr_t)hhdm_phys_to_virt(phys);
        memset(virt, 0, PAGE_SIZE);
        arch_set_storage_key(phys, (uint8_t)(skey << 4));
    }
}

/// @brief Unpoison a page on allocation: set its storage key to PMM_SKEY_KERNEL.
///        We do NOT zero the page here (zeroing happens only when ZX_GFP_ZERO is
///        set); the caller sees random data (which is expected behavior —
///        callers that need zeroed memory use ZX_GFP_ZERO or pmm_alloc_zeroed_page).
/// @param phys  Physical address of the 4 KB frame.
static inline void pmm_unpoison_page(uint64_t phys) {
    arch_set_storage_key(phys, (uint8_t)(PMM_SKEY_KERNEL << 4));
}

/// @brief Push a block to the HEAD of the doubly-linked free list.
///        Caller holds node_area->node_lock.
///        Also sets PF_BUDDY | PF_POISON and updates the free_bitmap.
static void node_list_push_head(pmm_node_area_t *na, uint32_t order, uint64_t pfn) {
    pmm_free_area_t *fa = &na->free_area[order];
    zx_page_t *page = pfn_to_page(pfn);

    page->buddy_prev = PMM_INVALID_PFN64;
    page->buddy_next = fa->head_pfn;

    if (fa->head_pfn != PMM_INVALID_PFN64)
        pfn_to_page(fa->head_pfn)->buddy_prev = pfn;
    else
        fa->tail_pfn = pfn;   // list was empty; new head is also the tail

    fa->head_pfn = pfn;
    fa->count++;

    // Mark flags: PF_BUDDY marks the page as in the free list;
    // PF_POISON marks it as hardware-poisoned (PFMF was applied before this call).
    page->flags = PF_BUDDY | PF_POISON;
    page->order = (uint8_t)order;

    na->free_pages += (1ULL << order);
    na->free_bitmap |= (1U << order);
}

/// @brief Pop a block from the HEAD of the doubly-linked free list.
///        Returns PMM_INVALID_PFN64 if the list is empty.
///        Caller holds node_area->node_lock.
///        Clears PF_BUDDY | PF_POISON; does NOT clear the hardware poison
///        (the caller does that after releasing the lock).
static uint64_t node_list_pop_head(pmm_node_area_t *na, uint32_t order) {
    pmm_free_area_t *fa = &na->free_area[order];
    if (fa->head_pfn == PMM_INVALID_PFN64)
        return PMM_INVALID_PFN64;

    uint64_t pfn = fa->head_pfn;
    zx_page_t *page = pfn_to_page(pfn);

    fa->head_pfn = page->buddy_next;
    if (fa->head_pfn != PMM_INVALID_PFN64)
        pfn_to_page(fa->head_pfn)->buddy_prev = PMM_INVALID_PFN64;
    else
        fa->tail_pfn = PMM_INVALID_PFN64; // list is now empty

    fa->count--;
    if (fa->count == 0)
        na->free_bitmap &= ~(1U << order);

    // Clear the page's free-list links and flags (hardware poison stays
    // until the caller calls pmm_unpoison_page() outside the lock).
    page->buddy_next = 0;
    page->buddy_prev = 0;
    page->flags &= ~(PF_BUDDY | PF_POISON);

    na->free_pages -= (1ULL << order);
    return pfn;
}

/// @brief Remove a specific PFN from the doubly-linked free list in O(1).
///        The caller must verify that the page is actually in the list
///        (PF_BUDDY set) before calling.
///        Caller holds node_area->node_lock.
static void node_list_remove(pmm_node_area_t *na, uint32_t order, uint64_t pfn) {
    pmm_free_area_t *fa = &na->free_area[order];
    zx_page_t *page = pfn_to_page(pfn);

    if (page->buddy_prev != PMM_INVALID_PFN64)
        pfn_to_page(page->buddy_prev)->buddy_next = page->buddy_next;
    else
        fa->head_pfn = page->buddy_next;  // pfn was the head

    if (page->buddy_next != PMM_INVALID_PFN64)
        pfn_to_page(page->buddy_next)->buddy_prev = page->buddy_prev;
    else
        fa->tail_pfn = page->buddy_prev;  // pfn was the tail

    fa->count--;
    if (fa->count == 0)
        na->free_bitmap &= ~(1U << order);

    page->buddy_next = 0;
    page->buddy_prev = 0;
    page->flags &= ~(PF_BUDDY | PF_POISON);

    na->free_pages -= (1ULL << order);
}

static inline zone_id_t pfn_to_zone_id(uint64_t pfn) {
    return (pmm_pfn_to_phys(pfn) < ZONE_DMA_LIMIT) ? ZONE_DMA : ZONE_NORMAL;
}

extern uint8_t __bss_end[];

typedef struct {
    uint64_t start;
    uint64_t end;
} pmm_resv_t;

bool is_critical_resv(uint64_t phys, uint32_t nresv, pmm_resv_t *resv) {
    for (uint32_t r = 0; r < nresv; r++) {
        if (phys >= resv[r].end)   continue;
        if (phys >= resv[r].start) return true;
        break; // sorted: no later entry can match
    }
    return false;
}

void pmm_init(const zxfl_boot_protocol_t *boot) {
    if (!boot)
        zx_system_check(ZX_SYSCHK_CORE_UNINITIALIZED, "pmm_init: missing boot protocol");

    memset(cpu_to_node_table, 0, sizeof(cpu_to_node_table));
    if (boot->flags & ZXFL_FLAG_SMP) {
        for (uint32_t i = 0; i < boot->cpu_count; i++) {
            uint16_t cpu_addr = boot->cpu_map[i].cpu_addr;
            uint8_t  node     = boot->cpu_map[i].numa_node;
            if (node >= NUMA_MAX_NODES) node = 0;

            // BSP always gets logical ID 0.
            if (cpu_addr == boot->bsp_cpu_addr) {
                cpu_to_node_table[0] = node;
            } else if (i < MAX_CPUS) {
                cpu_to_node_table[i] = node;
            }
        }
    }

    uint64_t bss_phys   = hhdm_virt_to_phys((uintptr_t)__bss_end);
    uint64_t kernel_end = (boot->kernel_phys_end > bss_phys)
                          ? boot->kernel_phys_end : bss_phys;

    printk(ZX_DEBUG "pmm: kernel_phys_end=0x%llx bss_phys=0x%llx pgtbl_pool_end=0x%llx\n",
           (unsigned long long)boot->kernel_phys_end,
           (unsigned long long)bss_phys,
           (unsigned long long)boot->pgtbl_pool_end);

    for (int z = 0; z < ZONE_MAX; z++) {
        spin_lock_init(&zones[z].lock);
        zones[z].id             = (zone_id_t)z;
        zones[z].free_pages     = 0;
        zones[z].atomic_reserve = PMM_ATOMIC_RESERVE;

        for (uint32_t n = 0; n < NUMA_MAX_NODES; n++) {
            pmm_node_area_t *na = &zones[z].nodes[n];
            spin_lock_init(&na->node_lock);
            na->free_bitmap     = 0;
            na->free_pages      = 0;
            na->suspect_pages   = 0;
            na->offline_pages   = 0;
            na->present         = false;
            na->node_id         = (uint8_t)n;
            list_init(&na->suspect_list);
            list_init(&na->offline_list);

            for (uint32_t o = 0; o < PMM_NR_ORDERS; o++) {
                na->free_area[o].head_pfn = PMM_INVALID_PFN64;
                na->free_area[o].tail_pfn = PMM_INVALID_PFN64;
                na->free_area[o].count    = 0;
            }

            na->watermark[PMM_WMARK_LOW]  = 64U;
            na->watermark[PMM_WMARK_HIGH] = 256U;
        }
    }

    zones[ZONE_DMA].pfn_start    = 0;
    zones[ZONE_DMA].pfn_end      = pmm_phys_to_pfn(ZONE_DMA_LIMIT);
    zones[ZONE_NORMAL].pfn_start = pmm_phys_to_pfn(ZONE_DMA_LIMIT);
    zones[ZONE_NORMAL].pfn_end   = 0; // set below after max_pfn is known

    if (!(boot->flags & ZXFL_FLAG_MEM_MAP) || !boot->mem_map_count)
        zx_system_check(ZX_SYSCHK_CORE_UNINITIALIZED,
                        "pmm_init: no memory map in boot protocol");

    const zxfl_mem_region_t *map =
        (const zxfl_mem_region_t *)(uintptr_t)boot->mem_map_addr;

    // --- Pass 0: Find max PFN and mark node presence ---
    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        uint64_t pfn_end = pmm_phys_to_pfn(map[i].base + map[i].length);
        if (pfn_end > max_pfn_global)
            max_pfn_global = pfn_end;

        if (map[i].type == ZXFL_MEM_USABLE) {
            uint8_t node = map[i].numa_node;
            if (node >= NUMA_MAX_NODES) node = 0;
            if (map[i].base < ZONE_DMA_LIMIT)
                zones[ZONE_DMA].nodes[node].present = true;
            if (map[i].base + map[i].length > ZONE_DMA_LIMIT)
                zones[ZONE_NORMAL].nodes[node].present = true;
        }
    }
    zones[ZONE_NORMAL].pfn_end = max_pfn_global;

    uint64_t map_bytes = max_pfn_global * sizeof(zx_page_t);
    uint64_t map_pages = (map_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t map_phys  = (kernel_end + PAGE_SIZE - 1) & PAGE_MASK;
    if (boot->pgtbl_pool_end > map_phys)
        map_phys = (boot->pgtbl_pool_end + PAGE_SIZE - 1) & PAGE_MASK;

    zx_mem_map = (zx_page_t *)(uintptr_t)hhdm_phys_to_virt(map_phys);
    memset(zx_mem_map, 0, map_pages * PAGE_SIZE);

    uint64_t reserve_phys_end = map_phys + map_pages * PAGE_SIZE;
    if (boot->pgtbl_pool_end > reserve_phys_end)
        reserve_phys_end = boot->pgtbl_pool_end;

    printk(ZX_DEBUG "pmm: zx_mem_map @ phys 0x%llx, reserve_end=0x%llx\n",
           (unsigned long long)map_phys,
           (unsigned long long)reserve_phys_end);

#define PMM_MAX_RESERVATIONS  (8U + ZXFL_MAX_MODULES)
    pmm_resv_t resv[PMM_MAX_RESERVATIONS];
    uint32_t   nresv = 0;

    resv[nresv++] = (pmm_resv_t){ 0,                   1024ULL * 1024ULL };
    resv[nresv++] = (pmm_resv_t){ boot->kernel_phys_start, boot->kernel_phys_end };
    resv[nresv++] = (pmm_resv_t){ map_phys,            map_phys + map_pages * PAGE_SIZE };
    if (boot->pgtbl_pool_end > boot->kernel_phys_end)
        resv[nresv++] = (pmm_resv_t){ boot->kernel_phys_end, boot->pgtbl_pool_end };

    for (uint32_t m = 0;
         m < boot->module_count && nresv < PMM_MAX_RESERVATIONS; m++) {
        resv[nresv++] = (pmm_resv_t){
            boot->modules[m].phys_start,
            boot->modules[m].phys_start + boot->modules[m].size_bytes
        };
    }

    // Sort reservation table by start address (insertion sort — small N).
    for (uint32_t i = 1; i < nresv; i++) {
        pmm_resv_t key = resv[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && resv[j].start > key.start) {
            resv[j + 1] = resv[j];
            j--;
        }
        resv[j + 1] = key;
    }


    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        const zxfl_mem_region_t *r = &map[i];
        if (r->type != ZXFL_MEM_USABLE) continue;

        uint8_t  node       = r->numa_node;
        if (node >= NUMA_MAX_NODES) node = 0;

        uint64_t start_phys = r->base;
        uint64_t end_phys   = r->base + r->length;
        uint64_t curr       = start_phys;

        while (curr < end_phys) {
            if (is_critical_resv(curr, nresv, resv)) {
                // This page is in a reserved range; mark it so.
                uint64_t   pfn  = pmm_phys_to_pfn(curr);
                zx_page_t *page = pfn_to_page(pfn);
                page->flags     = PF_RESERVED;
                page->order     = 0;
                page->zone_id   = (uint8_t)pfn_to_zone_id(pfn);
                page->numa_node = node;
                atomic_set(&page->refcount, 1);
                // Assign kernel storage key to reserved frames.
                arch_set_storage_key(curr, (uint8_t)(PMM_SKEY_KERNEL << 4));
                curr += PAGE_SIZE;
                continue;
            }

            // Find the end of the contiguous free run starting at curr.
            uint64_t run_end = curr + PAGE_SIZE;
            while (run_end < end_phys && !is_critical_resv(run_end, nresv, resv))
                run_end += PAGE_SIZE;
            // [curr, run_end) is a contiguous free range.

            // Decompose into power-of-two buddy blocks.
            uint64_t run_pfn     = pmm_phys_to_pfn(curr);
            uint64_t run_end_pfn = pmm_phys_to_pfn(run_end);

            while (run_pfn < run_end_pfn) {
                uint32_t order = MAX_ORDER;
                while (order > 0) {
                    uint64_t size = 1ULL << order;
                    if ((run_pfn & (size - 1)) == 0 &&
                        run_pfn + size <= run_end_pfn)
                        break;
                    order--;
                }

                uint64_t   block_pages = 1ULL << order;
                zx_page_t *head        = pfn_to_page(run_pfn);
                zone_id_t  zid         = pfn_to_zone_id(run_pfn);

                // Initialize head page.
                head->order     = (uint8_t)order;
                head->zone_id   = (uint8_t)zid;
                head->numa_node = node;
                head->flags     = 0;
                atomic_set(&head->refcount, 0);

                // Initialize sub-pages.
                for (uint64_t bpfn = run_pfn + 1;
                     bpfn < run_pfn + block_pages; bpfn++) {
                    zx_page_t *sub = pfn_to_page(bpfn);
                    sub->flags     = 0;
                    sub->order     = 0;
                    sub->zone_id   = (uint8_t)zid;
                    sub->numa_node = node;
                    atomic_set(&sub->refcount, 0);
                }

                for (uint64_t bpfn = run_pfn;
                     bpfn < run_pfn + block_pages; bpfn++) {
                    pmm_pfmf_zero_and_key(pmm_pfn_to_phys(bpfn), PMM_SKEY_POISON);
                }

                // Insert at head of the free list.
                pmm_node_area_t *na = &zones[zid].nodes[node];
                node_list_push_head(na, order, run_pfn);
                zones[zid].free_pages += block_pages;

                run_pfn += block_pages;
            }

            curr = run_end;
        }
    }
#undef PMM_MAX_RESERVATIONS

    // --- Log node presence ---
    for (int z = 0; z < ZONE_MAX; z++) {
        for (uint32_t n = 0; n < NUMA_MAX_NODES; n++) {
            pmm_node_area_t *na = &zones[z].nodes[n];
            if (na->present) {
                printk(ZX_DEBUG "pmm: zone %d node %u: %llu free pages\n",
                       z, n, (unsigned long long)na->free_pages);
            }
        }
    }

    pmm_ready = true;

    pmm_stats_t st;
    pmm_get_stats(&st);
    printk(ZX_INFO
           "pmm: %llu MB total, %llu MB free (%llu MB DMA, %llu MB normal)\n",
           (unsigned long long)(st.total_pages   * PAGE_SIZE / (1024*1024)),
           (unsigned long long)(st.free_pages    * PAGE_SIZE / (1024*1024)),
           (unsigned long long)(st.dma_free_pages    * PAGE_SIZE / (1024*1024)),
           (unsigned long long)(st.normal_free_pages * PAGE_SIZE / (1024*1024)));
}

void pmm_verify_hhdm(const zxfl_boot_protocol_t *boot) {
    if (!boot) return;
    printk(ZX_DEBUG "pmm: verifying HHDM consistency...\n");

    const bool has_edat1 = arch_cpu_has_sys_feature(ZX_SYS_FEATURE_EDAT1);
    const zxfl_mem_region_t *mem_map =
        (const zxfl_mem_region_t *)(uintptr_t)boot->mem_map_addr;

    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        const zxfl_mem_region_t *e = &mem_map[i];
        if (e->type != ZXFL_MEM_USABLE && e->type != ZXFL_MEM_KERNEL)
            continue;

        uint64_t start = e->base;
        uint64_t end   = start + e->length;

        // Check first and last 4 KB of each 1 MB segment.
        for (uint64_t p = start; p < end; p += (1ULL << 20)) {
            uint64_t virt      = hhdm_phys_to_virt(p);
            uint64_t phys_back = mmu_virt_to_phys(mmu_kernel_pgtbl(), virt);

            if (phys_back != p) {
                zx_system_check(ZX_SYSCHK_ARCH_CORRUPT,
                    "pmm: HHDM mismatch at phys 0x%llx (got 0x%llx)",
                    (unsigned long long)p,
                    (unsigned long long)phys_back);
            }

            // Verify last 4 KB of this 1 MB segment.
            uint64_t last = p + (1ULL << 20) - PAGE_SIZE;
            if (last < end) {
                uint64_t vl = hhdm_phys_to_virt(last);
                uint64_t pl = mmu_virt_to_phys(mmu_kernel_pgtbl(), vl);
                if (pl != last) {
                    zx_system_check(ZX_SYSCHK_ARCH_CORRUPT,
                        "pmm: HHDM tail mismatch at phys 0x%llx (got 0x%llx)",
                        (unsigned long long)last,
                        (unsigned long long)pl);
                }
            }

            if (has_edat1 && !mmu_is_large_page(mmu_kernel_pgtbl(), virt)) {
                printk(ZX_DEBUG
                       "pmm: HHDM at 0x%llx not using 1 MB pages despite EDAT1\n",
                       (unsigned long long)virt);
            }
        }
    }
    printk(ZX_INFO "pmm: HHDM consistency verified\n");
}

// ---------------------------------------------------------------------------
// CPU-to-node / local-node helpers
// ---------------------------------------------------------------------------

uint8_t pmm_cpu_to_node(uint16_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return 0;
    return cpu_to_node_table[cpu_id];
}

static inline uint8_t pmm_local_node(void) {
    return cpu_to_node_table[(uint16_t)arch_smp_processor_id() & (MAX_CPUS - 1)];
}

static uint64_t zone_node_alloc_locked(pmm_zone_t *zone, pmm_node_area_t *na,
                                       uint32_t order, gfp_t gfp) {
    if (!na->present) return PMM_INVALID_PFN64;

    if (!(gfp & ZX_GFP_ATOMIC) &&
        zone->free_pages <= zone->atomic_reserve + (1ULL << order))
        return PMM_INVALID_PFN64;

    uint32_t mask = na->free_bitmap >> order;
    if (mask == 0) return PMM_INVALID_PFN64;
    uint32_t found = order + (uint32_t)__builtin_ctz(mask);
    if (found > MAX_ORDER) return PMM_INVALID_PFN64;

    uint64_t pfn = node_list_pop_head(na, found);
    if (pfn == PMM_INVALID_PFN64) return PMM_INVALID_PFN64;

    // Deduct the full block from the zone counter.
    zone->free_pages -= (1ULL << found);

    while (found > order) {
        found--;
        uint64_t   buddy_pfn  = pfn + (1ULL << found);
        zx_page_t *buddy_page = pfn_to_page(buddy_pfn);
        buddy_page->order     = (uint8_t)found;
        buddy_page->zone_id   = pfn_to_page(pfn)->zone_id;
        buddy_page->numa_node = na->node_id;
        node_list_push_head(na, found, buddy_pfn);
        zone->free_pages += (1ULL << found);
    }

    return pfn;
}

zx_page_t *pmm_alloc_pages_node(uint8_t node, uint32_t order, gfp_t gfp) {
    if (!pmm_ready)
        zx_system_check(ZX_SYSCHK_CORE_UNINITIALIZED,
                        "pmm_alloc_pages_node: PMM not ready");
    if (order > MAX_ORDER) {
        printk(ZX_ERROR "pmm: order %u > MAX_ORDER\n", order);
        return nullptr;
    }

    // Resolve node.
    if (node == NUMA_NODE_ANY || node >= NUMA_MAX_NODES)
        node = pmm_local_node();

    // Determine preferred zone and fallback.
    zone_id_t preferred = (gfp & ZX_GFP_DMA) ? ZONE_DMA : ZONE_NORMAL;
    
    zone_id_t zones_to_try[ZONE_MAX];
    uint32_t  nzones = 0;

    zones_to_try[nzones++] = preferred;

    if (preferred == ZONE_NORMAL) {
        zones_to_try[nzones++] = ZONE_DMA;
    }

    for (uint32_t zidx = 0; zidx < nzones; zidx++) {
        zone_id_t   zid  = zones_to_try[zidx];
        pmm_zone_t *zone = &zones[zid];

        // Try the preferred node first, then fall back to other nodes.
        uint64_t   pfn  = PMM_INVALID_PFN64;
        uint8_t    used_node = node;
        irqflags_t flags;

        for (uint32_t attempt = 0; attempt < NUMA_MAX_NODES; attempt++) {
            uint8_t try_node = (uint8_t)((node + attempt) % NUMA_MAX_NODES);
            pmm_node_area_t *na = &zones[zid].nodes[try_node];
            if (!na->present) continue;

            if (!(gfp & ZX_GFP_NOIRQ))
                spin_lock_irqsave(&na->node_lock, &flags);
            else
                spin_lock(&na->node_lock);

            pfn = zone_node_alloc_locked(zone, na, order, gfp);

            if (!(gfp & ZX_GFP_NOIRQ))
                spin_unlock_irqrestore(&na->node_lock, flags);
            else
                spin_unlock(&na->node_lock);

            if (pfn != PMM_INVALID_PFN64) {
                used_node = try_node;
                break;
            }
        }

        if (pfn == PMM_INVALID_PFN64) continue; // try next zone

        zx_page_t *page = pfn_to_page(pfn);

        page->order     = (uint8_t)order;
        page->zone_id   = (uint8_t)zid;
        page->numa_node = used_node;
        page->flags     = 0;           // clears PF_BUDDY, PF_POISON, etc.
        atomic_set(&page->refcount, 1);

        for (uint64_t sub = 0; sub < (1ULL << order); sub++) {
            pmm_unpoison_page(pmm_pfn_to_phys(pfn + sub));
        }

        // 3. Zero if requested.
        if (gfp & ZX_GFP_ZERO) {
            uint8_t *virt = (uint8_t *)(uintptr_t)
                hhdm_phys_to_virt(pmm_pfn_to_phys(pfn));
            memset(virt, 0, (size_t)PAGE_SIZE << order);
        }

        return page;
    }

    return nullptr; // OOM
}

zx_page_t *pmm_alloc_pages(uint32_t order, gfp_t gfp) {
    uint8_t node = ZX_GFP_HAS_NODE(gfp) ? gfp_to_node(gfp) : NUMA_NODE_ANY;
    return pmm_alloc_pages_node(node, order, gfp);
}

/// @brief Batch-refill the hot magazine from the buddy allocator.
///        Acquires the node_lock once for PCP_BATCH allocations.
///        Called with IRQs disabled (already done by the PCP fast path).
static void pcp_batch_refill(pmm_pcplist_t *pcp, zone_id_t zid) {
    uint8_t     node  = pmm_local_node();
    pmm_zone_t *zone  = &zones[zid];
    pmm_node_area_t *na = &zones[zid].nodes[node];

    // Use NOIRQ because IRQs are already disabled at the call site.
    spin_lock(&na->node_lock);

    uint32_t filled = 0;
    while (filled < PCP_BATCH) {
        uint64_t pfn = zone_node_alloc_locked(zone, na, 0, ZX_GFP_NOIRQ);
        if (pfn == PMM_INVALID_PFN64) break;
        pcp->hot[pcp->count_hot + filled] = pfn;
        filled++;
    }

    spin_unlock(&na->node_lock);

    // Unpoison the batch outside the lock.
    for (uint32_t i = 0; i < filled; i++) {
        uint64_t pfn = pcp->hot[pcp->count_hot + i];
        pmm_unpoison_page(pmm_pfn_to_phys(pfn));
        pfn_to_page(pfn)->flags = 0;
        atomic_set(&pfn_to_page(pfn)->refcount, 1);
    }
    pcp->count_hot += filled;
    pcp->numa_node  = node;
}

/// @brief Batch-drain PCP_BATCH pages from the cold magazine to the buddy.
///        Called with IRQs disabled.
static void pcp_batch_drain(pmm_pcplist_t *pcp) {
    uint32_t drain = (pcp->count_cold > PCP_BATCH) ? PCP_BATCH : pcp->count_cold;
    if (drain == 0) return;

    // Poison all pages before returning to the buddy.
    for (uint32_t i = 0; i < drain; i++) {
        uint64_t pfn = pcp->cold[pcp->count_cold - 1 - i];
        pmm_pfmf_zero_and_key(pmm_pfn_to_phys(pfn), PMM_SKEY_POISON);
    }

    // Acquire zone lock once for the batch free.
    uint8_t     node  = pcp->numa_node < NUMA_MAX_NODES ? pcp->numa_node : 0;
    zone_id_t   zid   = (zone_id_t)pcp->zone_id;
    pmm_zone_t *zone  = &zones[zid];
    pmm_node_area_t *na = &zone->nodes[node];

    spin_lock(&na->node_lock);
    for (uint32_t i = 0; i < drain; i++) {
        uint64_t   pfn  = pcp->cold[--pcp->count_cold];
        zx_page_t *page = pfn_to_page(pfn);

        // Coalesce with buddy before inserting.
        uint32_t ord = 0;
        while (ord < MAX_ORDER) {
            uint64_t   buddy_pfn  = pfn ^ (1ULL << ord);
            if (buddy_pfn >= max_pfn_global) break;
            zx_page_t *buddy = pfn_to_page(buddy_pfn);
            // A buddy is coalesceable iff it is free (PF_BUDDY), at the same
            // order, in the same zone and node.
            if (!(buddy->flags & PF_BUDDY))      break;
            if (buddy->order    != ord)           break;
            if (buddy->zone_id  != (uint8_t)zid) break;
            if (buddy->numa_node != node)         break;

            node_list_remove(na, ord, buddy_pfn);
            zone->free_pages -= (1ULL << ord);
            pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
            page = pfn_to_page(pfn);
            ord++;
        }

        page->order     = (uint8_t)ord;
        page->zone_id   = (uint8_t)zid;
        page->numa_node = node;
        atomic_set(&page->refcount, 0);
        node_list_push_head(na, ord, pfn);
        zone->free_pages += (1ULL << ord);
    }
    spin_unlock(&na->node_lock);
}

// ---------------------------------------------------------------------------
// pmm_alloc_page — order-0 fast path via PCP
// ---------------------------------------------------------------------------

zx_page_t *pmm_alloc_page_node(uint8_t node, gfp_t gfp) {
    return pmm_alloc_pages_node(node, 0, gfp);
}

zx_page_t *pmm_alloc_page(gfp_t gfp) {
    // ZX_GFP_ATOMIC bypasses PCP entirely — straight to buddy with reserve.
    if ((gfp & ZX_GFP_ATOMIC) || !pmm_ready)
        goto slow;

    {
        zone_id_t  zid = (gfp & ZX_GFP_DMA) ? ZONE_DMA : ZONE_NORMAL;
        irqflags_t f   = arch_local_save_flags();
        arch_local_irq_disable();

        int cpu = arch_smp_processor_id();
        if ((uint32_t)cpu >= MAX_CPUS || !zx_lowcore_cpu(cpu)) {
            arch_local_irq_restore(f);
            goto slow;
        }

        pmm_pcplist_t *pcp = &zx_lowcore_cpu(cpu)->percpu.pcp[zid];

        // Try hot magazine.
        if (pcp->count_hot > 0) {
            uint64_t   pfn = pcp->hot[--pcp->count_hot];
            arch_local_irq_restore(f);
            zx_page_t *pg  = pfn_to_page(pfn);
            pmm_unpoison_page(pmm_pfn_to_phys(pfn));
            pg->flags = 0;
            atomic_set(&pg->refcount, 1);
            if (gfp & ZX_GFP_ZERO) {
                void *va = page_to_virt(pg);
                memset(va, 0, PAGE_SIZE);
            }
            return pg;
        }

        // Hot is empty — swap hot/cold if cold has pages.
        if (pcp->count_cold > 0) {
            for (uint32_t j = 0; j < pcp->count_cold; j++) {
                pcp->hot[j] = pcp->cold[j];
            }
            pcp->count_hot  = pcp->count_cold;
            pcp->count_cold = 0;

            uint64_t   pfn = pcp->hot[--pcp->count_hot];
            arch_local_irq_restore(f);
            pmm_unpoison_page(pmm_pfn_to_phys(pfn));
            zx_page_t *pg = pfn_to_page(pfn);
            pg->flags = 0;
            atomic_set(&pg->refcount, 1);
            if (gfp & ZX_GFP_ZERO) {
                void *va = page_to_virt(pg);
                memset(va, 0, PAGE_SIZE);
            }
            return pg;
        }

        // Both magazines empty — refill hot from buddy.
        pcp_batch_refill(pcp, zid);
        if (pcp->count_hot > 0) {
            uint64_t   pfn = pcp->hot[--pcp->count_hot];
            arch_local_irq_restore(f);
            // Already unpoisoned by pcp_batch_refill.
            zx_page_t *pg = pfn_to_page(pfn);
            pg->flags = 0;
            atomic_set(&pg->refcount, 1);
            if (gfp & ZX_GFP_ZERO) {
                void *va = page_to_virt(pg);
                memset(va, 0, PAGE_SIZE);
            }
            return pg;
        }

        arch_local_irq_restore(f);
    }

slow:
    {
        uint8_t node = ZX_GFP_HAS_NODE(gfp) ? gfp_to_node(gfp) : pmm_local_node();
        return pmm_alloc_pages_node(node, 0, gfp);
    }
}

zx_page_t *pmm_alloc_zeroed_page(uint8_t node, gfp_t gfp) {
    gfp_t req_gfp = gfp & ~(gfp_t)ZX_GFP_ZERO; // ZX_GFP_ZERO already implied
    zx_page_t *page = pmm_alloc_page_node(node, req_gfp);
    if (page)
        page->flags |= PF_ZERO_READY;
    return page;
}

uint32_t pmm_alloc_batch(uint8_t node, gfp_t gfp,
                         zx_page_t **pages_out, uint32_t max_pages) {
    if (!max_pages) return 0;

    uint32_t    allocated = 0;
    uint8_t     use_node  = (node == NUMA_NODE_ANY || node >= NUMA_MAX_NODES)
                            ? pmm_local_node() : node;
    zone_id_t   zid       = (gfp & ZX_GFP_DMA) ? ZONE_DMA : ZONE_NORMAL;
    pmm_zone_t *zone      = &zones[zid];
    pmm_node_area_t *na   = &zone->nodes[use_node];

    irqflags_t flags;
    if (!(gfp & ZX_GFP_NOIRQ))
        spin_lock_irqsave(&na->node_lock, &flags);
    else
        spin_lock(&na->node_lock);

    while (allocated < max_pages) {
        uint64_t pfn = zone_node_alloc_locked(zone, na, 0, gfp);
        if (pfn == PMM_INVALID_PFN64) break;
        pages_out[allocated++] = pfn_to_page(pfn);
    }

    if (!(gfp & ZX_GFP_NOIRQ))
        spin_unlock_irqrestore(&na->node_lock, flags);
    else
        spin_unlock(&na->node_lock);

    // Unpoison and initialize outside the lock.
    for (uint32_t i = 0; i < allocated; i++) {
        zx_page_t *pg  = pages_out[i];
        uint64_t   pfn = page_to_pfn(pg);
        pmm_unpoison_page(pmm_pfn_to_phys(pfn));
        pg->flags = (gfp & ZX_GFP_ZERO) ? PF_ZERO_READY : 0;
        atomic_set(&pg->refcount, 1);
        if (gfp & ZX_GFP_ZERO) {
            void *va = page_to_virt(pg);
            memset(va, 0, PAGE_SIZE);
        }
    }

    return allocated;
}

void pmm_free_pages(zx_page_t *page, uint32_t order) {
    if (!pmm_ready)
        zx_system_check(ZX_SYSCHK_CORE_UNINITIALIZED,
                        "pmm_free_pages: PMM not initialized");
    if (!page) {
        printk(ZX_ERROR "pmm: pmm_free_pages: null page\n");
        return;
    }

    if (page->flags & PF_BUDDY)
        zx_system_check(ZX_SYSCHK_MEM_DOUBLE_FREE,
                        "pmm_free_pages: PFN %llu already in buddy",
                        (unsigned long long)page_to_pfn(page));
    if (page->flags & PF_POISON)
        zx_system_check(ZX_SYSCHK_MEM_DOUBLE_FREE,
                        "pmm_free_pages: PFN %llu is hardware-poisoned (double-free)",
                        (unsigned long long)page_to_pfn(page));
    if (page->flags & (PF_PINNED | PF_RESERVED | PF_OFFLINE))
        zx_system_check(ZX_SYSCHK_MEM_DOUBLE_FREE,
                        "pmm_free_pages: PFN %llu has forbidden flags 0x%x",
                        (unsigned long long)page_to_pfn(page), page->flags);
    if (order > MAX_ORDER)
        zx_system_check(ZX_SYSCHK_MEM_DOUBLE_FREE,
                        "pmm_free_pages: PFN %llu order %u > MAX_ORDER",
                        (unsigned long long)page_to_pfn(page), order);

    zone_id_t   zid  = (zone_id_t)page->zone_id;
    uint8_t     node = page->numa_node;
    if (node >= NUMA_MAX_NODES) node = 0;
    pmm_zone_t *zone = &zones[zid];
    uint64_t    pfn  = page_to_pfn(page);

    for (uint64_t sub = 0; sub < (1ULL << order); sub++)
        pmm_pfmf_zero_and_key(pmm_pfn_to_phys(pfn + sub), PMM_SKEY_POISON);

    pmm_node_area_t *na = &zone->nodes[node];
    irqflags_t irqf;
    spin_lock_irqsave(&na->node_lock, &irqf);

    uint32_t ord = order;

    while (ord < MAX_ORDER) {
        uint64_t   buddy_pfn  = pfn ^ (1ULL << ord);
        if (buddy_pfn >= max_pfn_global) break;
        zx_page_t *buddy = pfn_to_page(buddy_pfn);

        if (!(buddy->flags & PF_BUDDY))      break;
        if (buddy->order    != ord)          break;
        if (buddy->zone_id  != (uint8_t)zid) break;
        if (buddy->numa_node != node)        break;

        node_list_remove(na, ord, buddy_pfn);
        zone->free_pages -= (1ULL << ord);
        pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
        ord++;
    }

    zx_page_t *head = pfn_to_page(pfn);
    head->order     = (uint8_t)ord;
    head->zone_id   = (uint8_t)zid;
    head->numa_node = node;
    atomic_set(&head->refcount, 0);
    node_list_push_head(na, ord, pfn);
    zone->free_pages += (1ULL << ord);

    spin_unlock_irqrestore(&na->node_lock, irqf);
}

void pmm_free_page(zx_page_t *page) {
    if (!page) return;

    if (page->flags & (PF_BUDDY | PF_POISON))
        zx_system_check(ZX_SYSCHK_MEM_DOUBLE_FREE,
                        "pmm_free_page: PFN %llu already free",
                        (unsigned long long)page_to_pfn(page));
    if (page->flags & (PF_PINNED | PF_RESERVED | PF_OFFLINE))
        zx_system_check(ZX_SYSCHK_MEM_DOUBLE_FREE,
                        "pmm_free_page: PFN %llu has forbidden flags 0x%x",
                        (unsigned long long)page_to_pfn(page), page->flags);

    if (pmm_ready && !(page->flags & (PF_SLAB | PF_VMALLOC))) {
        irqflags_t f = arch_local_save_flags();
        arch_local_irq_disable();

        int cpu = arch_smp_processor_id();
        if ((uint32_t)cpu < MAX_CPUS && zx_lowcore_cpu(cpu)) {
            zone_id_t      zid = (zone_id_t)page->zone_id;
            pmm_pcplist_t *pcp = &zx_lowcore_cpu(cpu)->percpu.pcp[zid];

            if (pcp->count_hot < PCP_HIGH) {
                // Poison the page before storing in PCP.
                uint64_t pfn = page_to_pfn(page);
                page->flags  = 0;
                atomic_set(&page->refcount, 0);
                pmm_pfmf_zero_and_key(pmm_pfn_to_phys(pfn), PMM_SKEY_POISON);
                pcp->hot[pcp->count_hot++] = pfn;
                arch_local_irq_restore(f);
                return;
            }

            // Hot magazine full — swap hot to cold, then drain cold to buddy
            // if cold is also full.
            if (pcp->count_cold == 0) {
                // Cold is empty: move hot content to cold.
                for (uint32_t j = 0; j < pcp->count_hot; j++)
                    pcp->cold[j] = pcp->hot[j];
                pcp->count_cold = pcp->count_hot;
                pcp->count_hot  = 0;
            } else {
                // Both full — drain cold to buddy.
                pcp_batch_drain(pcp);
                // Move hot to cold.
                for (uint32_t j = 0; j < pcp->count_hot; j++)
                    pcp->cold[j] = pcp->hot[j];
                pcp->count_cold = pcp->count_hot;
                pcp->count_hot  = 0;
            }

            // Add current page to now-empty hot magazine.
            uint64_t pfn = page_to_pfn(page);
            page->flags  = 0;
            atomic_set(&page->refcount, 0);
            pmm_pfmf_zero_and_key(pmm_pfn_to_phys(pfn), PMM_SKEY_POISON);
            pcp->hot[pcp->count_hot++] = pfn;
            arch_local_irq_restore(f);
            return;
        }

        arch_local_irq_restore(f);
    }

    pmm_free_pages(page, 0);
}

// ---------------------------------------------------------------------------
// pmm_free_batch
// ---------------------------------------------------------------------------

uint32_t pmm_free_batch(zx_page_t **pages, uint32_t count) {
    if (!count) return 0;

    // Poison all pages first (outside any lock — PFMF is serializing).
    for (uint32_t i = 0; i < count; i++) {
        zx_page_t *pg  = pages[i];
        uint64_t   pfn = page_to_pfn(pg);

        if (pg->flags & (PF_BUDDY | PF_POISON | PF_PINNED | PF_RESERVED | PF_OFFLINE))
            zx_system_check(ZX_SYSCHK_MEM_DOUBLE_FREE,
                            "pmm_free_batch: bad page PFN %llu flags 0x%x",
                            (unsigned long long)pfn, pg->flags);

        pmm_pfmf_zero_and_key(pmm_pfn_to_phys(pfn), PMM_SKEY_POISON);
        pg->flags = 0;
        atomic_set(&pg->refcount, 0);
    }

    // Group by zone/node for a single lock acquisition per group.
    // For simplicity (bounded count ≤ PCP_BATCH * 4), free individually.
    for (uint32_t i = 0; i < count; i++) {
        zx_page_t *pg   = pages[i];
        zone_id_t  zid  = (zone_id_t)pg->zone_id;
        uint8_t    node = pg->numa_node;
        if (node >= NUMA_MAX_NODES) node = 0;

        pmm_zone_t      *zone = &zones[zid];
        pmm_node_area_t *na   = &zone->nodes[node];
        uint64_t         pfn  = page_to_pfn(pg);

        irqflags_t irqf;
        spin_lock_irqsave(&na->node_lock, &irqf);

        uint32_t ord = 0;
        while (ord < MAX_ORDER) {
            uint64_t   buddy_pfn = pfn ^ (1ULL << ord);
            if (buddy_pfn >= max_pfn_global) break;
            zx_page_t *buddy = pfn_to_page(buddy_pfn);
            if (!(buddy->flags & PF_BUDDY))      break;
            if (buddy->order    != ord)          break;
            if (buddy->zone_id  != (uint8_t)zid) break;
            if (buddy->numa_node != node)        break;
            node_list_remove(na, ord, buddy_pfn);
            zone->free_pages -= (1ULL << ord);
            pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
            pg  = pfn_to_page(pfn);
            ord++;
        }
        pg->order     = (uint8_t)ord;
        pg->zone_id   = (uint8_t)zid;
        pg->numa_node = node;
        atomic_set(&pg->refcount, 0);
        node_list_push_head(na, ord, pfn);
        zone->free_pages += (1ULL << ord);

        spin_unlock_irqrestore(&na->node_lock, irqf);
    }

    return count;
}

// ---------------------------------------------------------------------------
// PCP lifecycle
// ---------------------------------------------------------------------------

void pmm_pcplist_init(uint16_t cpu_id) {
    if (cpu_id >= MAX_CPUS || !zx_lowcore_cpu(cpu_id)) return;
    for (int z = 0; z < ZONE_MAX; z++) {
        pmm_pcplist_t *pcp = &zx_lowcore_cpu(cpu_id)->percpu.pcp[z];
        pcp->count_hot  = 0;
        pcp->count_cold = 0;
        pcp->zone_id    = (uint8_t)z;
        pcp->numa_node  = pmm_cpu_to_node(cpu_id);
    }
}

void pmm_drain_local_pcps(void) {
    int cpu = arch_smp_processor_id();
    if ((uint32_t)cpu >= MAX_CPUS || !zx_lowcore_cpu(cpu)) return;

    for (int z = 0; z < ZONE_MAX; z++) {
        pmm_pcplist_t *pcp = &zx_lowcore_cpu(cpu)->percpu.pcp[z];

        // Drain hot.
        while (pcp->count_hot > 0) {
            uint64_t pfn = pcp->hot[--pcp->count_hot];
            // Page is already poisoned (was poisoned on PCP insertion).
            zx_page_t *pg = pfn_to_page(pfn);
            zone_id_t  zid  = (zone_id_t)pg->zone_id;
            uint8_t    node = pg->numa_node;
            if (node >= NUMA_MAX_NODES) node = 0;
            pmm_zone_t      *zone = &zones[zid];
            pmm_node_area_t *na   = &zone->nodes[node];

            spin_lock(&na->node_lock);
            pg->order = 0;
            node_list_push_head(na, 0, pfn);
            zone->free_pages++;
            spin_unlock(&na->node_lock);
        }

        // Drain cold.
        while (pcp->count_cold > 0) {
            uint64_t pfn = pcp->cold[--pcp->count_cold];
            zx_page_t *pg = pfn_to_page(pfn);
            zone_id_t  zid  = (zone_id_t)pg->zone_id;
            uint8_t    node = pg->numa_node;
            if (node >= NUMA_MAX_NODES) node = 0;
            pmm_zone_t      *zone = &zones[zid];
            pmm_node_area_t *na   = &zone->nodes[node];

            spin_lock(&na->node_lock);
            pg->order = 0;
            node_list_push_head(na, 0, pfn);
            zone->free_pages++;
            spin_unlock(&na->node_lock);
        }
    }
}

void pmm_drain_all_pcps(void) {
    irqflags_t f = arch_local_save_flags();
    arch_local_irq_disable();
    pmm_drain_local_pcps();
    arch_local_irq_restore(f);

    // IPI all other CPUs to drain their PCPs.
    // The IPI handler calls pmm_drain_local_pcps() on each remote CPU.
    arch_ipi_broadcast_wait(IPI_DRAIN_PCP);
}

void pmm_cpu_offline_drain(uint16_t cpu_id) {
    // The offline CPU must have already disabled IRQs and stopped scheduling.
    // We directly access its lowcore and drain its PCP into the buddy.
    if (cpu_id >= MAX_CPUS || !zx_lowcore_cpu(cpu_id)) return;

    for (int z = 0; z < ZONE_MAX; z++) {
        pmm_pcplist_t *pcp = &zx_lowcore_cpu(cpu_id)->percpu.pcp[z];

        while (pcp->count_hot > 0) {
            uint64_t   pfn  = pcp->hot[--pcp->count_hot];
            zx_page_t *pg   = pfn_to_page(pfn);
            zone_id_t  zid  = (zone_id_t)pg->zone_id;
            uint8_t    node = pg->numa_node;
            if (node >= NUMA_MAX_NODES) node = 0;
            pmm_zone_t *zone = &zones[zid];
            pmm_node_area_t *na = &zone->nodes[node];
            irqflags_t irqf;
            spin_lock_irqsave(&na->node_lock, &irqf);
            pg->order = 0;
            node_list_push_head(na, 0, pfn);
            zone->free_pages++;
            spin_unlock_irqrestore(&na->node_lock, irqf);
        }

        while (pcp->count_cold > 0) {
            uint64_t   pfn  = pcp->cold[--pcp->count_cold];
            zx_page_t *pg   = pfn_to_page(pfn);
            zone_id_t  zid  = (zone_id_t)pg->zone_id;
            uint8_t    node = pg->numa_node;
            if (node >= NUMA_MAX_NODES) node = 0;
            pmm_zone_t *zone = &zones[zid];
            pmm_node_area_t *na = &zone->nodes[node];
            irqflags_t irqf;
            spin_lock_irqsave(&na->node_lock, &irqf);
            pg->order = 0;
            node_list_push_head(na, 0, pfn);
            zone->free_pages++;
            spin_unlock_irqrestore(&na->node_lock, irqf);
        }
    }
}

// ---------------------------------------------------------------------------
// pmm_reserve_range
// ---------------------------------------------------------------------------

void pmm_reserve_range(uint64_t phys_start, uint64_t phys_end) {
    if (!pmm_ready) return;

    // Drain all PCPs first — ensures no reserved page is cached anywhere.
    pmm_drain_all_pcps();

    uint64_t pfn_start = pmm_phys_to_pfn(phys_start);
    uint64_t pfn_end   = pmm_phys_to_pfn((phys_end + PAGE_SIZE - 1) & PAGE_MASK);
    if (pfn_end > max_pfn_global) pfn_end = max_pfn_global;

    for (uint64_t pfn = pfn_start; pfn < pfn_end; pfn++) {
        zx_page_t *p = pfn_to_page(pfn);
        if (!(p->flags & PF_BUDDY)) continue;

        zone_id_t   zid  = (zone_id_t)p->zone_id;
        uint8_t     node = p->numa_node;
        if (node >= NUMA_MAX_NODES) node = 0;
        pmm_zone_t *zone = &zones[zid];
        pmm_node_area_t *na = &zone->nodes[node];

        irqflags_t irqf;
        spin_lock_irqsave(&na->node_lock, &irqf);

        uint32_t ord        = p->order;
        uint64_t block_head = pfn & ~((1ULL << ord) - 1);

        // Verify p is actually the head of its block (it may be a sub-page
        // whose block head was already removed in a previous iteration).
        if (!(pfn_to_page(block_head)->flags & PF_BUDDY) ||
            pfn_to_page(block_head)->order != ord) {
            spin_unlock_irqrestore(&na->node_lock, irqf);
            continue;
        }

        node_list_remove(na, ord, block_head);
        zone->free_pages -= (1ULL << ord);

        // Split the block: return the parts that don't overlap the reserved
        // range back to the free list.
        while (ord > 0) {
            ord--;
            uint64_t lo = block_head;
            uint64_t hi = block_head + (1ULL << ord);

            if (pfn >= hi) {
                // The target page is in the high half; return the low half.
                zx_page_t *lo_page = pfn_to_page(lo);
                lo_page->order     = (uint8_t)ord;
                lo_page->zone_id   = (uint8_t)zid;
                lo_page->numa_node = node;
                node_list_push_head(na, ord, lo);
                zone->free_pages += (1ULL << ord);
                block_head = hi;
            } else {
                // The target page is in the low half; return the high half.
                zx_page_t *hi_page = pfn_to_page(hi);
                hi_page->order     = (uint8_t)ord;
                hi_page->zone_id   = (uint8_t)zid;
                hi_page->numa_node = node;
                node_list_push_head(na, ord, hi);
                zone->free_pages += (1ULL << ord);
            }
        }

        // Mark the isolated page as reserved.
        zx_page_t *rp = pfn_to_page(block_head);
        rp->flags  = PF_RESERVED;
        rp->order  = 0;
        atomic_set(&rp->refcount, 1);
        arch_set_storage_key(pmm_pfn_to_phys(block_head),
                             (uint8_t)(PMM_SKEY_KERNEL << 4));

        spin_unlock_irqrestore(&na->node_lock, irqf);
    }
}

// ---------------------------------------------------------------------------
// MCCK recovery
// ---------------------------------------------------------------------------

void pmm_mark_suspect(zx_page_t *page) {
    if (!page) return;

    // If already offline or suspect, do nothing.
    if (page->flags & (PF_OFFLINE | PF_SUSPECT)) return;

    zone_id_t   zid  = (zone_id_t)page->zone_id;
    uint8_t     node = page->numa_node;
    if (node >= NUMA_MAX_NODES) node = 0;
    pmm_zone_t      *zone = &zones[zid];
    pmm_node_area_t *na   = &zone->nodes[node];

    irqflags_t irqf;
    spin_lock_irqsave(&na->node_lock, &irqf);

    page->flags |= PF_SUSPECT;
    page->skey   = PMM_SKEY_SUSPECT;
    na->suspect_pages++;
    list_add_tail(&page->lru, &na->suspect_list);

    spin_unlock_irqrestore(&na->node_lock, irqf);

    // Assign the suspect storage key so the hardware can distinguish this frame.
    arch_set_storage_key(page_to_phys(page),
                         (uint8_t)(PMM_SKEY_SUSPECT << 4));

    printk(ZX_WARN "pmm: PFN %llu marked suspect (corrected storage error)\n",
           (unsigned long long)page_to_pfn(page));
}

bool pmm_offline_page(zx_page_t *page) {
    if (!page) return false;
    if (page->flags & PF_OFFLINE) return true; // already offlined

    zone_id_t   zid  = (zone_id_t)page->zone_id;
    uint8_t     node = page->numa_node;
    if (node >= NUMA_MAX_NODES) node = 0;
    pmm_zone_t      *zone = &zones[zid];
    pmm_node_area_t *na   = &zone->nodes[node];

    irqflags_t irqf;
    spin_lock_irqsave(&na->node_lock, &irqf);

    // If the page is currently allocated (refcount > 0), we cannot offline it
    // immediately.  Return false; the MCCK handler must retry after reclaim.
    if (atomic_read(&page->refcount) > 0) {
        spin_unlock_irqrestore(&na->node_lock, irqf);
        printk(ZX_WARN "pmm: PFN %llu in use — cannot offline immediately\n",
               (unsigned long long)page_to_pfn(page));
        return false;
    }

    // Remove from buddy if it is currently free.
    if (page->flags & PF_BUDDY) {
        uint64_t block_head = page_to_pfn(page) & ~((1ULL << page->order) - 1);
        node_list_remove(na, page->order, block_head);
        zone->free_pages -= (1ULL << page->order);
        na->free_pages   -= (1ULL << page->order);
    }

    // Remove from suspect_list if present.
    if (page->flags & PF_SUSPECT) {
        list_del(&page->lru);
        na->suspect_pages--;
    }

    page->flags  = PF_OFFLINE;
    page->order  = 0;
    atomic_set(&page->refcount, 1);
    na->offline_pages++;
    list_add_tail(&page->lru, &na->offline_list);

    spin_unlock_irqrestore(&na->node_lock, irqf);

    // Assign poison key to the offlined frame so any stray access is caught.
    arch_set_storage_key(page_to_phys(page), (uint8_t)(PMM_SKEY_POISON << 4));

    printk(ZX_WARN "pmm: PFN %llu permanently offlined (uncorrected storage error)\n",
           (unsigned long long)page_to_pfn(page));
    return true;
}

// ---------------------------------------------------------------------------
// Hardware storage-key assignment
// ---------------------------------------------------------------------------

void pmm_set_page_key(zx_page_t *page, uint32_t order, uint8_t skey) {
    if (!page) return;
    if (order > MAX_ORDER) return;

    uint64_t pfn_start = page_to_pfn(page);
    uint64_t pfn_end   = pfn_start + (1ULL << order);

    for (uint64_t pfn = pfn_start; pfn < pfn_end; pfn++) {
        arch_set_storage_key(pmm_pfn_to_phys(pfn), (uint8_t)(skey << 4));
        pfn_to_page(pfn)->skey = skey;
    }

    page->flags |= PF_KEY_SET;
}

void pmm_set_key_range(uint64_t pfn_start, uint64_t count, uint8_t skey) {
    uint8_t key_byte = (uint8_t)(skey << 4);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t phys = pmm_pfn_to_phys(pfn_start + i);
        arch_set_storage_key(phys, key_byte);
        pfn_to_page(pfn_start + i)->skey = skey;
    }
}

// ---------------------------------------------------------------------------
// Watermark query
// ---------------------------------------------------------------------------

bool pmm_zone_below_watermark(zone_id_t zid, uint8_t node, pmm_wmark_t wm) {
    if ((unsigned)zid >= ZONE_MAX) return false;

    if (node == NUMA_NODE_ANY || node >= NUMA_MAX_NODES) {
        // Aggregate across all nodes.
        uint64_t total_free = zones[zid].free_pages;
        uint64_t total_wm   = 0;
        for (uint32_t n = 0; n < NUMA_MAX_NODES; n++) {
            if (zones[zid].nodes[n].present)
                total_wm += zones[zid].nodes[n].watermark[wm];
        }
        return total_free < total_wm;
    }

    pmm_node_area_t *na = &zones[zid].nodes[node];
    return na->free_pages < na->watermark[wm];
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void pmm_get_stats(pmm_stats_t *out) {
    out->dma_free_pages    = 0;
    out->normal_free_pages = 0;
    out->free_pages        = 0;
    out->suspect_pages     = 0;
    out->offline_pages     = 0;

    for (uint32_t n = 0; n < NUMA_MAX_NODES; n++) {
        out->node_free_pages[n]    = 0;
        out->node_suspect_pages[n] = 0;
    }

    uint64_t total = 0;

    for (int z = 0; z < ZONE_MAX; z++) {
        irqflags_t f;
        spin_lock_irqsave(&zones[z].lock, &f);

        uint64_t fp = zones[z].free_pages;
        uint64_t tp = zones[z].pfn_end - zones[z].pfn_start;

        for (uint32_t n = 0; n < NUMA_MAX_NODES; n++) {
            out->node_free_pages[n]    += zones[z].nodes[n].free_pages;
            out->node_suspect_pages[n] += zones[z].nodes[n].suspect_pages;
            out->suspect_pages         += zones[z].nodes[n].suspect_pages;
            out->offline_pages         += zones[z].nodes[n].offline_pages;
        }

        spin_unlock_irqrestore(&zones[z].lock, f);

        if (z == ZONE_DMA)    out->dma_free_pages    = fp;
        if (z == ZONE_NORMAL) out->normal_free_pages = fp;
        total            += tp;
        out->free_pages  += fp;
    }

    out->total_pages    = total;
    out->reserved_pages = total - out->free_pages
                          - out->suspect_pages
                          - out->offline_pages;
}

uint64_t pmm_get_max_pfn(void) { return max_pfn_global; }

pmm_zone_t *pmm_get_zone(zone_id_t id) {
    if ((unsigned)id >= ZONE_MAX) return nullptr;
    return &zones[id];
}

zx_page_t *pmm_dma_alloc(uint32_t order, gfp_t gfp) {
    return pmm_alloc_pages(order, gfp | ZX_GFP_DMA);
}

void pmm_dma_free(zx_page_t *page, uint32_t order) {
    pmm_free_pages(page, order);
}
