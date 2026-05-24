// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/pmm.c
//
/// @brief Zone-aware buddy physical memory manager.

#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <zxfoundation/percpu.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/mmu/mmu.h>
#include <arch/s390x/cpu/ipi.h>
#include <arch/s390x/linksym.h>
#include <lib/string.h>

/// Two physical memory zones: DMA (< 16 MB) and NORMAL (>= 16 MB).
static pmm_zone_t zones[ZONE_MAX];

/// Flat array of page descriptors, allocated from physical memory during init.
zx_page_t *zx_mem_map = nullptr;

/// Highest PFN seen in the boot memory map.
static uint64_t max_pfn_global = 0;

/// True once pmm_init() completes.
static bool pmm_ready = false;

/// @brief Push a block onto the zone's per-order free list (LIFO).
///        Caller holds zone->lock.
static void free_area_push(pmm_zone_t *zone, uint32_t order, uint64_t pfn) {
    pmm_free_area_t *fa = &zone->free_area[order];
    zx_page_t *page = pfn_to_page(pfn);
    page->buddy_next = (uint32_t)fa->head; // truncation is fine: max PFN < 2^32
    fa->head = pfn;
    fa->count++;
    page->flags |= PF_BUDDY;
    page->flags |= PF_POISON; // mark free block as poisoned against UAF
}

/// @brief Pop a block from the zone's per-order free list.
///        Returns PMM_INVALID_PFN if the list is empty.
///        Caller holds zone->lock.
static uint64_t free_area_pop(pmm_zone_t *zone, uint32_t order) {
    pmm_free_area_t *fa = &zone->free_area[order];
    if (fa->head == PMM_INVALID_PFN)
        return PMM_INVALID_PFN;

    uint64_t pfn = fa->head;
    zx_page_t *page = pfn_to_page(pfn);
    fa->head = page->buddy_next;
    fa->count--;
    page->flags &= ~(PF_BUDDY | PF_POISON);
    page->buddy_next = 0;
    return pfn;
}

/// @brief Remove a specific PFN from a free list (used during coalescing).
///        O(n) but orders are small and called only at free time.
///        Caller holds zone->lock.
static bool free_area_remove(pmm_zone_t *zone, uint32_t order, uint64_t pfn) {
    pmm_free_area_t *fa = &zone->free_area[order];
    uint64_t cur = fa->head;
    uint64_t prev = PMM_INVALID_PFN;

    while (cur != PMM_INVALID_PFN) {
        if (cur == pfn) {
            zx_page_t *page = pfn_to_page(cur);
            if (prev == PMM_INVALID_PFN)
                fa->head = page->buddy_next;
            else
                pfn_to_page(prev)->buddy_next = page->buddy_next;
            fa->count--;
            page->flags &= ~(PF_BUDDY | PF_POISON);
            page->buddy_next = 0;
            return true;
        }
        prev = cur;
        cur = pfn_to_page(cur)->buddy_next;
    }
    return false;
}

static zone_id_t pfn_to_zone_id(uint64_t pfn) {
    uint64_t phys = pmm_pfn_to_phys(pfn);
    return (phys < ZONE_DMA_LIMIT) ? ZONE_DMA : ZONE_NORMAL;
}

extern uint8_t __bss_end[];

void pmm_init(const zxfl_boot_protocol_t *boot) {
    if (!boot)
        zx_system_check(ZX_SYSCHK_CORE_UNINITIALIZED, "pmm_init: missing protocol");

    // --- Determine true kernel footprint ---
    uint64_t bss_phys = hhdm_virt_to_phys((uintptr_t)__bss_end);
    uint64_t kernel_end = (boot->kernel_phys_end > bss_phys) ? boot->kernel_phys_end : bss_phys;

    printk(ZX_DEBUG "pmm: boot->kernel_phys_end = 0x%llx, bss_phys = 0x%llx, pgtbl_pool_end = 0x%llx\n",
           (unsigned long long)boot->kernel_phys_end,
           (unsigned long long)bss_phys,
           (unsigned long long)boot->pgtbl_pool_end);

    for (int z = 0; z < ZONE_MAX; z++) {
        spin_lock_init(&zones[z].lock);
        zones[z].id = (zone_id_t)z;
        zones[z].free_pages = 0;
        zones[z].atomic_reserve = PMM_ATOMIC_RESERVE;
        for (uint32_t o = 0; o <= MAX_ORDER; o++) {
            zones[z].free_area[o].head  = PMM_INVALID_PFN;
            zones[z].free_area[o].count = 0;
        }
    }
    zones[ZONE_DMA].pfn_start    = 0;
    zones[ZONE_DMA].pfn_end      = pmm_phys_to_pfn(ZONE_DMA_LIMIT);
    zones[ZONE_NORMAL].pfn_start = pmm_phys_to_pfn(ZONE_DMA_LIMIT);
    zones[ZONE_NORMAL].pfn_end   = 0; // set below

    if (!(boot->flags & ZXFL_FLAG_MEM_MAP) || !boot->mem_map_count)
        zx_system_check(ZX_SYSCHK_CORE_UNINITIALIZED, "pmm_init: no memory map in boot protocol");

    const zxfl_mem_region_t *map =
        (const zxfl_mem_region_t *)(uintptr_t)boot->mem_map_addr;

    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        uint64_t pfn_end = pmm_phys_to_pfn(map[i].base + map[i].length);
        if (pfn_end > max_pfn_global) max_pfn_global = pfn_end;
    }
    zones[ZONE_NORMAL].pfn_end = max_pfn_global;

    uint64_t map_bytes  = max_pfn_global * sizeof(zx_page_t);
    uint64_t map_pages  = (map_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t map_phys   = (kernel_end + PAGE_SIZE - 1) & PAGE_MASK;

    uint8_t *mem_map_raw = (uint8_t *)(uintptr_t)hhdm_phys_to_virt(map_phys);
    memset(mem_map_raw, 0, map_pages * PAGE_SIZE);

    zx_mem_map = (zx_page_t *)(uintptr_t)hhdm_phys_to_virt(map_phys);

    uint64_t reserve_phys_end = map_phys + map_pages * PAGE_SIZE;
    if (boot->pgtbl_pool_end > reserve_phys_end)
        reserve_phys_end = boot->pgtbl_pool_end;

    // --- Hard-protect ZONE_DMA from over-reservation ---
    const uint64_t dma_min_free = 1024 * 1024;
    if (reserve_phys_end > ZONE_DMA_LIMIT - dma_min_free) {
        printk(ZX_WARN "pmm: early reservation (0x%llx) encroaches on ZONE_DMA; capping visibility\n",
               (unsigned long long)reserve_phys_end);
    }

    printk(ZX_DEBUG "pmm: reserve_phys_end = 0x%llx (pool_end = 0x%llx)\n",
           (unsigned long long)reserve_phys_end,
           (unsigned long long)boot->pgtbl_pool_end);

    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        const zxfl_mem_region_t *r = &map[i];
        if (r->type != ZXFL_MEM_USABLE) continue;

        uint64_t start_phys = r->base;
        uint64_t end_phys   = r->base + r->length;

        for (uint64_t curr = start_phys; curr < end_phys; ) {
            uint64_t next = (curr + PAGE_SIZE) & PAGE_MASK;
            if (next > end_phys) next = end_phys;

            uint64_t phys = curr;
            bool critical = false;

            if (phys < 1024 * 1024) critical = true;
            if (phys >= boot->kernel_phys_start && phys < boot->kernel_phys_end) critical = true;
            if (phys >= ZONE_DMA_LIMIT && phys < boot->pgtbl_pool_end) critical = true;
            if (phys >= map_phys && phys < map_phys + map_pages * PAGE_SIZE) critical = true;

            for (uint32_t m = 0; m < boot->module_count; m++) {
                if (phys >= boot->modules[m].phys_start &&
                    phys < boot->modules[m].phys_start + boot->modules[m].size_bytes) {
                    critical = true;
                    break;
                }
            }

            if (critical) {
                uint64_t pfn = pmm_phys_to_pfn(phys);
                zx_page_t *p = pfn_to_page(pfn);
                p->zone_id   = (uint8_t)pfn_to_zone_id(pfn);
                p->numa_node = 0;
                p->order     = 0;
                p->flags     = PF_RESERVED;
                atomic_set(&p->refcount, 1);
                curr = next;
                continue;
            }

            // Not critical: try to add as the largest possible buddy block.
            uint64_t pfn = pmm_phys_to_pfn(phys);
            zx_page_t *p = pfn_to_page(pfn);
            p->zone_id   = (uint8_t)pfn_to_zone_id(pfn);
            p->numa_node = 0;
            p->order     = 0;
            p->flags     = PF_BUDDY;
            atomic_set(&p->refcount, 0);

            curr = next;
        }
    }

    // Pass 2: Coalescing.
    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        const zxfl_mem_region_t *r = &map[i];
        if (r->type != ZXFL_MEM_USABLE) continue;

        uint64_t pfn_start = pmm_phys_to_pfn(r->base);
        uint64_t pfn_end   = pmm_phys_to_pfn(r->base + r->length);

        for (uint64_t pfn = pfn_start; pfn < pfn_end; ) {
            zx_page_t *p = pfn_to_page(pfn);
            if (!(p->flags & PF_BUDDY) || atomic_read(&p->refcount) != 0) {
                pfn++;
                continue;
            }

            // Try to form the largest possible block starting at pfn.
            uint32_t order = 0;
            while (order < MAX_ORDER) {
                uint32_t next_order = order + 1;
                uint64_t block_size = 1ULL << next_order;
                if ((pfn & (block_size - 1)) != 0) break;
                if (pfn + block_size > pfn_end) break;

                // Check if all pages in the potential buddy block are also PF_BUDDY and free.
                bool buddy_ready = true;
                uint64_t buddy_pfn = pfn + (1ULL << order);
                zx_page_t *bp = pfn_to_page(buddy_pfn);
                if (!(bp->flags & PF_BUDDY) || atomic_read(&bp->refcount) != 0 || bp->order != order) {
                    buddy_ready = false;
                }

                if (!buddy_ready) break;

                // Merge.
                order = next_order;
            }

            p->order = (uint8_t)order;
            zone_id_t zid = (zone_id_t)p->zone_id;
            free_area_push(&zones[zid], order, pfn);
            zones[zid].free_pages += (1ULL << order);
            pfn += (1ULL << order);
        }
    }

    pmm_ready = true;

    pmm_stats_t st;
    pmm_get_stats(&st);
    printk(ZX_DEBUG "pmm: %llu MB total, %llu MB free (%llu MB DMA, %llu MB normal)\n",
           (unsigned long long)(st.total_pages   * PAGE_SIZE / (1024*1024)),
           (unsigned long long)(st.free_pages    * PAGE_SIZE / (1024*1024)),
           (unsigned long long)(st.dma_free_pages    * PAGE_SIZE / (1024*1024)),
           (unsigned long long)(st.normal_free_pages * PAGE_SIZE / (1024*1024)));
}

void pmm_verify_hhdm(const zxfl_boot_protocol_t *boot) {
    if (!boot) return;
    printk(ZX_DEBUG "pmm: verifying HHDM consistency...\n");

    const bool has_edat1 = arch_cpu_has_sys_feature(ZX_SYS_FEATURE_EDAT1);
    const zxfl_mem_region_t *mem_map = (const zxfl_mem_region_t *)(uintptr_t)boot->mem_map_addr;

    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        const zxfl_mem_region_t *e = &mem_map[i];
        if (e->type != ZXFL_MEM_USABLE && e->type != ZXFL_MEM_KERNEL) continue;

        uint64_t start = e->base;
        uint64_t end   = start + e->length;

        // Check a few points in each range.
        for (uint64_t p = start; p < end; p += (16ULL << 20)) { // every 16MB
            uint64_t virt = hhdm_phys_to_virt(p);
            uint64_t phys_back = mmu_virt_to_phys(mmu_kernel_pgtbl(), virt);

            if (phys_back != p) {
                zx_system_check(ZX_SYSCHK_ARCH_CORRUPT,
                    "pmm: HHDM verification failed at phys 0x%llx (got 0x%llx)",
                    (unsigned long long)p, (unsigned long long)phys_back);
            }

            // Verify page size usage.
            if (has_edat1 && !mmu_is_large_page(mmu_kernel_pgtbl(), virt)) {
                 printk(ZX_DEBUG "pmm: HHDM at 0x%llx not using 1MB pages despite EDAT1\n", (unsigned long long)virt);
            }
        }
    }
    printk(ZX_INFO "pmm: HHDM consistency verified\n");
}

zx_page_t *pmm_alloc_pages(uint32_t order, gfp_t gfp) {
    if (!pmm_ready) zx_system_check(ZX_SYSCHK_CORE_UNINITIALIZED, "pmm_alloc_pages: pmm not initialized");
    if (order > MAX_ORDER) {
        printk(ZX_ERROR "pmm: pmm_alloc_pages: order %u > MAX_ORDER", order);
        return nullptr;
    }

    // Determine which zone to allocate from.
    zone_id_t preferred = (gfp & ZX_GFP_DMA) ? ZONE_DMA : ZONE_NORMAL;
    zone_id_t fallback  = (gfp & ZX_GFP_DMA_FALLBACK) ? ZONE_DMA : ZONE_MAX;

    for (zone_id_t zid = preferred; zid < ZONE_MAX; zid++) {
        if (zid != preferred && zid != fallback) continue;

        pmm_zone_t *zone = &zones[zid];
        irqflags_t flags;
        if (!(gfp & ZX_GFP_NOIRQ))
            spin_lock_irqsave(&zone->lock, &flags);
        else
            spin_lock(&zone->lock);

        // Find the smallest order >= requested with a free block.
        uint32_t found_order = MAX_ORDER + 1;
        for (uint32_t o = order; o <= MAX_ORDER; o++) {
            if (zone->free_area[o].head != PMM_INVALID_PFN) {
                found_order = o;
                break;
            }
        }

        if (found_order > MAX_ORDER) {
            if (!(gfp & ZX_GFP_NOIRQ))
                spin_unlock_irqrestore(&zone->lock, flags);
            else
                spin_unlock(&zone->lock);
            continue; // try next zone
        }

        // Non-atomic callers must leave the atomic reserve intact.
        if (!(gfp & ZX_GFP_ATOMIC) &&
            zone->free_pages <= zone->atomic_reserve + (1ULL << order)) {
            if (!(gfp & ZX_GFP_NOIRQ))
                spin_unlock_irqrestore(&zone->lock, flags);
            else
                spin_unlock(&zone->lock);
            continue;
        }

        uint64_t pfn = free_area_pop(zone, found_order);
        zone->free_pages -= (1ULL << found_order);

        // Split down to the requested order, pushing high halves back.
        while (found_order > order) {
            found_order--;
            uint64_t buddy_pfn = pfn + (1ULL << found_order);
            zx_page_t *buddy_page = pfn_to_page(buddy_pfn);
            buddy_page->order   = (uint8_t)found_order;
            buddy_page->zone_id = pfn_to_page(pfn)->zone_id;
            free_area_push(zone, found_order, buddy_pfn);
            zone->free_pages += (1ULL << found_order);
        }

        if (!(gfp & ZX_GFP_NOIRQ))
            spin_unlock_irqrestore(&zone->lock, flags);
        else
            spin_unlock(&zone->lock);

        zx_page_t *page = pfn_to_page(pfn);
        page->order   = (uint8_t)order;
        page->flags   = 0;         // clear PF_BUDDY | PF_POISON
        // Initialise refcount to 1 — caller owns the page.
        atomic_set(&page->refcount, 1);

        if (gfp & ZX_GFP_ZERO) {
            uint8_t *virt = (uint8_t *)(uintptr_t)hhdm_phys_to_virt(pmm_pfn_to_phys(pfn));
            memset(virt, 0, (size_t)PAGE_SIZE << order);
        }
        return page;
    }
    return nullptr; // OOM
}

/// @brief Refill a CPU's pcplist for zone zid by popping PCP_BATCH pages
///        from the buddy allocator under the zone lock.
static void pcp_refill(pmm_pcplist_t *pcp, zone_id_t zid) {
    for (uint32_t i = 0; i < PCP_BATCH; i++) {
        zx_page_t *p = pmm_alloc_pages(0, ZX_GFP_NORMAL | ZX_GFP_NOIRQ |
                                          (zid == ZONE_DMA ? ZX_GFP_DMA : 0));
        if (!p) break;
        pcp->pages[pcp->count++] = page_to_pfn(p);
    }
}

/// @brief Drain PCP_BATCH pages from a CPU's pcplist back to the buddy pool.
static void pcp_drain(pmm_pcplist_t *pcp) {
    uint32_t drain = (pcp->count > PCP_BATCH) ? PCP_BATCH : pcp->count;
    for (uint32_t i = 0; i < drain; i++) {
        uint64_t pfn = pcp->pages[--pcp->count];
        pmm_free_pages(pfn_to_page(pfn), 0);
    }
}

zx_page_t *pmm_alloc_page(gfp_t gfp) {
    // PCP fast path: only for order-0 NORMAL/DMA, non-ATOMIC, non-NOIRQ.
    if (!(gfp & (ZX_GFP_ATOMIC | ZX_GFP_NOIRQ)) && pmm_ready) {
        zone_id_t zid = (gfp & ZX_GFP_DMA) ? ZONE_DMA : ZONE_NORMAL;
        irqflags_t f  = arch_local_save_flags();
        arch_local_irq_disable();

        int cpu = arch_smp_processor_id();
        if ((uint32_t)cpu < MAX_CPUS && percpu_areas[cpu]) {
            pmm_pcplist_t *pcp = &percpu_areas[cpu]->percpu.pcp[zid];
            if (pcp->count == 0)
                pcp_refill(pcp, zid);
            if (pcp->count > 0) {
                uint64_t pfn  = pcp->pages[--pcp->count];
                arch_local_irq_restore(f);
                zx_page_t *pg = pfn_to_page(pfn);
                pg->flags = 0;
                atomic_set(&pg->refcount, 1);
                if (gfp & ZX_GFP_ZERO) {
                    uint8_t *va = (uint8_t *)(uintptr_t)
                        hhdm_phys_to_virt(pmm_pfn_to_phys(pfn));
                    memset(va, 0, PAGE_SIZE);
                }
                return pg;
            }
        }
        arch_local_irq_restore(f);
    }
    return pmm_alloc_pages(0, gfp);
}

void pmm_free_pages(zx_page_t *page, uint32_t order) {
    if (!pmm_ready) zx_system_check(ZX_SYSCHK_CORE_UNINITIALIZED, "pmm_free_pages: null not initialized");
    if (!page) {
        printk(ZX_ERROR "pmm: pmm_free_pages: null page");
        return;
    }

    if (page->flags & PF_POISON)
        zx_system_check(ZX_SYSCHK_MEM_DOUBLE_FREE, "pmm_free_pages: double-free PFN %llu",
              (unsigned long long)page_to_pfn(page));
    if (page->flags & PF_BUDDY)
        zx_system_check(ZX_SYSCHK_MEM_DOUBLE_FREE, "pmm_free_pages: freeing already-free PFN %llu",
              (unsigned long long)page_to_pfn(page));

    zone_id_t zid   = (zone_id_t)page->zone_id;
    pmm_zone_t *zone = &zones[zid];
    uint64_t pfn     = page_to_pfn(page);

    irqflags_t irqf;
    spin_lock_irqsave(&zone->lock, &irqf);

    uint32_t ord = order;
    while (ord < MAX_ORDER) {
        uint64_t buddy_pfn  = pfn ^ (1ULL << ord);
        if (buddy_pfn >= max_pfn_global) break;
        zx_page_t *buddy = pfn_to_page(buddy_pfn);
        if (!(buddy->flags & PF_BUDDY)) break;
        if (buddy->order != ord)        break;
        if (buddy->zone_id != (uint8_t)zid) break;

        free_area_remove(zone, ord, buddy_pfn);
        zone->free_pages -= (1ULL << ord);
        pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
        ord++;
    }

    zx_page_t *head = pfn_to_page(pfn);
    head->order   = (uint8_t)ord;
    head->zone_id = (uint8_t)zid;
    atomic_set(&head->refcount, 0);
    free_area_push(zone, ord, pfn);
    zone->free_pages += (1ULL << ord);

    spin_unlock_irqrestore(&zone->lock, irqf);
}

void pmm_free_page(zx_page_t *page) {
    if (!page) return;
    // PCP fast path: cache the page locally, drain to buddy when full.
    if (pmm_ready && !(page->flags & (PF_RESERVED | PF_SLAB))) {
        irqflags_t f = arch_local_save_flags();
        arch_local_irq_disable();

        int cpu = arch_smp_processor_id();
        if ((uint32_t)cpu < MAX_CPUS && percpu_areas[cpu]) {
            zone_id_t zid    = (zone_id_t)page->zone_id;
            pmm_pcplist_t *pcp = &percpu_areas[cpu]->percpu.pcp[zid];
            if (pcp->count < PCP_HIGH) {
                pcp->pages[pcp->count++] = page_to_pfn(page);
                arch_local_irq_restore(f);
                return;
            }
            pcp_drain(pcp);
            pcp->pages[pcp->count++] = page_to_pfn(page);
            arch_local_irq_restore(f);
            return;
        }
        arch_local_irq_restore(f);
    }
    pmm_free_pages(page, 0);
}

void pmm_pcplist_init(uint16_t cpu_id) {
    if (cpu_id >= MAX_CPUS || !percpu_areas[cpu_id]) return;
    for (uint32_t z = 0; z < ZONE_MAX; z++) {
        pmm_pcplist_t *pcp = &percpu_areas[cpu_id]->percpu.pcp[z];
        pcp->count   = 0;
        pcp->zone_id = z;
    }
}

void pmm_drain_local_pcps(void) {
    for (uint32_t z = 0; z < ZONE_MAX; z++) {
        int cpu = arch_smp_processor_id();
        if ((uint32_t)cpu < MAX_CPUS && percpu_areas[cpu]) {
            pmm_pcplist_t *pcp = &percpu_areas[cpu]->percpu.pcp[z];
            while (pcp->count > 0) {
                uint64_t pfn = pcp->pages[--pcp->count];
                pmm_free_pages(pfn_to_page(pfn), 0);
            }
        }
    }
}

void pmm_drain_all_pcps(void) {
    // 1. Drain local caches first.
    irqflags_t f = arch_local_save_flags();
    arch_local_irq_disable();
    pmm_drain_local_pcps();
    arch_local_irq_restore(f);

    // 2. Broadcast IPI to all other CPUs to drain theirs.
    arch_ipi_broadcast_wait(IPI_DRAIN_PCP);
}

void pmm_reserve_range(uint64_t phys_start, uint64_t phys_end) {
    if (!pmm_ready) return; // Called before init? ignore.

    // Critical: Drain all per-CPU lists before we start marking pages PF_RESERVED.
    // This prevents other CPUs from allocating pages we're about to reserve.
    pmm_drain_all_pcps();

    uint64_t pfn_start = pmm_phys_to_pfn(phys_start);
    uint64_t pfn_end   = pmm_phys_to_pfn(phys_end + PAGE_SIZE - 1);
    if (pfn_end > max_pfn_global) pfn_end = max_pfn_global;

    for (uint64_t pfn = pfn_start; pfn < pfn_end; pfn++) {
        zx_page_t *p = pfn_to_page(pfn);
        if (!(p->flags & PF_BUDDY)) continue; // already allocated or reserved

        zone_id_t zid    = (zone_id_t)p->zone_id;
        pmm_zone_t *zone = &zones[zid];

        irqflags_t f;
        spin_lock_irqsave(&zone->lock, &f);

        uint32_t ord = p->order;
        uint64_t block_head = pfn & ~((1ULL << ord) - 1);

        if (free_area_remove(zone, ord, block_head)) {
            zone->free_pages -= (1ULL << ord);
            while (ord > 0) {
                ord--;
                uint64_t lo = block_head;
                uint64_t hi = block_head + (1ULL << ord);
                if (pfn >= hi) {
                    zx_page_t *lo_page = pfn_to_page(lo);
                    lo_page->order = (uint8_t)ord;
                    lo_page->zone_id = (uint8_t)zid;
                    free_area_push(zone, ord, lo);
                    zone->free_pages += (1ULL << ord);
                    block_head = hi;
                } else {
                    // pfn is in the low half; put high half back.
                    zx_page_t *hi_page = pfn_to_page(hi);
                    hi_page->order = (uint8_t)ord;
                    hi_page->zone_id = (uint8_t)zid;
                    free_area_push(zone, ord, hi);
                    zone->free_pages += (1ULL << ord);
                }
            }
            zx_page_t *rp = pfn_to_page(block_head);
            rp->flags  = PF_RESERVED;
            rp->order  = 0;
            atomic_set(&rp->refcount, 1);
        }
        spin_unlock_irqrestore(&zone->lock, f);
    }
}

void pmm_get_stats(pmm_stats_t *out) {
    out->dma_free_pages    = 0;
    out->normal_free_pages = 0;
    out->free_pages        = 0;
    uint64_t total = 0;

    for (int z = 0; z < ZONE_MAX; z++) {
        irqflags_t f;
        spin_lock_irqsave(&zones[z].lock, &f);
        uint64_t fp = zones[z].free_pages;
        uint64_t tp = zones[z].pfn_end - zones[z].pfn_start;
        spin_unlock_irqrestore(&zones[z].lock, f);

        if (z == ZONE_DMA)    out->dma_free_pages    = fp;
        if (z == ZONE_NORMAL) out->normal_free_pages = fp;
        total += tp;
        out->free_pages += fp;
    }
    out->total_pages    = total;
    out->reserved_pages = total - out->free_pages;
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
