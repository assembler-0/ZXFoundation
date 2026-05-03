// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/pmm.c
//
/// @brief Zone-aware buddy physical memory manager.
///
///        BUDDY ALGORITHM OVERVIEW
///        ========================
///        Frames are managed in power-of-two blocks called "orders"
///        (order 0 = 4 KB, order MAX_ORDER = 4 MB).  Each zone keeps
///        one free_area[] for every order.  A free_area is an intrusive
///        singly-linked list of block heads; the link lives inside the
///        zx_page_t::buddy_next field (a PFN, not a pointer, so the list
///        survives HHDM remap without fixup).
///
///        ALLOCATION (pmm_alloc_pages):
///          1. Find the smallest order >= requested that has a free block.
///          2. If that order is larger than requested, split the block
///             repeatedly, pushing the high half ("buddy") back onto the
///             smaller-order free list each time.
///
///        FREEING (pmm_free_pages):
///          1. Mark the block free.
///          2. Compute the buddy PFN: buddy_pfn = pfn ^ (1 << order).
///          3. If the buddy is also free and at the same order, remove it
///             from the free list and coalesce, incrementing order.
///          4. Repeat step 2-3 up to MAX_ORDER.
///
///        SMP SAFETY
///        ==========
///        Each zone has its own ticket spinlock.  All paths acquire that
///        lock with irqsave so that interrupt handlers cannot re-enter.
///        The lock is released before returning to the caller.
///
///        UAF DETECTION
///        =============
///        PF_POISON is set when a block is freed.  pmm_free_pages() panics
///        if PF_POISON is already set (double-free) or if PF_BUDDY is set
///        without PF_POISON (freeing an already-free but not-yet-reused block).

#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/spinlock.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <lib/string.h>

// ---------------------------------------------------------------------------
// Module-private state
// ---------------------------------------------------------------------------

/// Two physical memory zones: DMA (< 16 MB) and NORMAL (>= 16 MB).
static pmm_zone_t zones[ZONE_MAX];

/// Flat array of page descriptors, allocated from physical memory during init.
zx_page_t *zx_mem_map = nullptr;

/// Highest PFN seen in the boot memory map.
static uint64_t max_pfn_global = 0;

/// True once pmm_init() completes.
static bool pmm_ready = false;

// ---------------------------------------------------------------------------
// Internal free-list helpers
// ---------------------------------------------------------------------------

/// @brief Push a block onto the zone's per-order free list (LIFO).
///        Caller holds zone->lock.
static void free_area_push(pmm_zone_t *zone, uint32_t order, uint64_t pfn) {
    pmm_free_area_t *fa = &zone->free_area[order];
    zx_page_t *page = pfn_to_page(pfn);
    page->buddy_next = (uint32_t)fa->head; // truncation is fine: max PFN < 2^32
    // Store the upper 32 bits of the PFN in the flags word's scratch bits.
    // For systems < 4 TB (max physical), PFN fits in 32 bits, so buddy_next
    // is sufficient. For future 64-bit PFN support a secondary field would
    // be added; today PMM_MAX_PHYS_PAGES is bounded at init time.
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

// ---------------------------------------------------------------------------
// Zone helpers
// ---------------------------------------------------------------------------

static zone_id_t pfn_to_zone_id(uint64_t pfn) {
    uint64_t phys = pmm_pfn_to_phys(pfn);
    return (phys < ZONE_DMA_LIMIT) ? ZONE_DMA : ZONE_NORMAL;
}

// ---------------------------------------------------------------------------
// pmm_init()
// ---------------------------------------------------------------------------

extern uint8_t __bss_end[];

void pmm_init(const zxfl_boot_protocol_t *boot) {
    if (!boot)
        panic("pmm_init: missing protocol");

    // --- Determine true kernel footprint ---
    uint64_t bss_phys = hhdm_virt_to_phys((uintptr_t)__bss_end);
    uint64_t kernel_end = (boot->kernel_phys_end > bss_phys) ? boot->kernel_phys_end : bss_phys;

    // --- Zero-initialise zone structures ---
    for (int z = 0; z < ZONE_MAX; z++) {
        spin_lock_init(&zones[z].lock);
        zones[z].id = (zone_id_t)z;
        zones[z].free_pages = 0;
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
        panic("pmm_init: no memory map in boot protocol");

    const zxfl_mem_region_t *map =
        (const zxfl_mem_region_t *)(uintptr_t)boot->mem_map_addr;

    // --- Determine max PFN ---
    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        uint64_t pfn_end = pmm_phys_to_pfn(map[i].base + map[i].length);
        if (pfn_end > max_pfn_global) max_pfn_global = pfn_end;
    }
    zones[ZONE_NORMAL].pfn_end = max_pfn_global;

    // --- Allocate mem_map from early physical memory ---
    // We need a flat array of zx_page_t[max_pfn_global].
    // Place it just above the kernel image, page-aligned.
    uint64_t map_bytes  = max_pfn_global * sizeof(zx_page_t);
    uint64_t map_pages  = (map_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t map_phys   = (kernel_end + PAGE_SIZE - 1) & PAGE_MASK;

    // Zero the mem_map region using byte writes (no memset available yet).
    uint8_t *mem_map_raw = (uint8_t *)(uintptr_t)hhdm_phys_to_virt(map_phys);
    for (uint64_t b = 0; b < map_pages * PAGE_SIZE; b++)
        mem_map_raw[b] = 0;

    zx_mem_map = (zx_page_t *)(uintptr_t)hhdm_phys_to_virt(map_phys);

    // --- Compute the end of the reserved early region ---
    uint64_t reserve_phys_end = map_phys + map_pages * PAGE_SIZE;

    // --- Walk the boot memory map and populate the buddy free lists ---
    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        const zxfl_mem_region_t *r = &map[i];
        if (r->type != ZXFL_MEM_USABLE) continue;

        uint64_t pfn_start = pmm_phys_to_pfn(r->base);
        uint64_t pfn_end   = pmm_phys_to_pfn(r->base + r->length);

        // Skip frames covered by lowcore + kernel image + mem_map.
        uint64_t reserve_pfn_end = pmm_phys_to_pfn(reserve_phys_end + PAGE_SIZE - 1);
        if (pfn_end <= reserve_pfn_end) continue;
        if (pfn_start < reserve_pfn_end) pfn_start = reserve_pfn_end;

        // Add each frame to its zone's order-0 free list.
        // Immediately try to coalesce into higher orders.
        for (uint64_t pfn = pfn_start; pfn < pfn_end; pfn++) {
            zx_page_t *p = pfn_to_page(pfn);
            p->zone_id   = (uint8_t)pfn_to_zone_id(pfn);
            p->numa_node = 0;
            p->order     = 0;
            p->flags     = 0;

            // Coalesce upward: try to merge with the buddy at each order.
            uint32_t ord = 0;
            uint64_t cur = pfn;
            while (ord < MAX_ORDER) {
                uint64_t buddy = cur ^ (1ULL << ord);
                if (buddy >= max_pfn_global) break;
                zx_page_t *buddy_page = pfn_to_page(buddy);
                // Buddy must be free, same order, same zone.
                if (!(buddy_page->flags & PF_BUDDY)) break;
                if (buddy_page->order != ord) break;
                if (buddy_page->zone_id != p->zone_id) break;

                zone_id_t zid = (zone_id_t)p->zone_id;
                free_area_remove(&zones[zid], ord, buddy);
                // The merged block head is the lower PFN.
                cur = (cur < buddy) ? cur : buddy;
                pfn_to_page(cur)->order = (uint8_t)(ord + 1);
                ord++;
            }
            pfn_to_page(cur)->order = (uint8_t)ord;
            zone_id_t zid = (zone_id_t)pfn_to_page(cur)->zone_id;
            free_area_push(&zones[zid], ord, cur);
            zones[zid].free_pages += 1;
        }
    }

    pmm_ready = true;

    pmm_stats_t st;
    pmm_get_stats(&st);
    printk("pmm: %llu MB total, %llu MB free (%llu MB DMA, %llu MB normal)\n",
           (unsigned long long)(st.total_pages   * PAGE_SIZE / (1024*1024)),
           (unsigned long long)(st.free_pages    * PAGE_SIZE / (1024*1024)),
           (unsigned long long)(st.dma_free_pages    * PAGE_SIZE / (1024*1024)),
           (unsigned long long)(st.normal_free_pages * PAGE_SIZE / (1024*1024)));
}

// ---------------------------------------------------------------------------
// Core alloc / free
// ---------------------------------------------------------------------------

zx_page_t *pmm_alloc_pages(uint32_t order, gfp_t gfp) {
    if (!pmm_ready) panic("pmm_alloc_pages: PMM not initialised");
    if (order > MAX_ORDER) panic("pmm_alloc_pages: order %u > MAX_ORDER", order);

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

        // Pop the block from found_order.
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
            uint64_t sz   = (uint64_t)PAGE_SIZE << order;
            for (uint64_t b = 0; b < sz; b++) virt[b] = 0;
        }
        return page;
    }
    return nullptr; // OOM
}

zx_page_t *pmm_alloc_page(gfp_t gfp) {
    return pmm_alloc_pages(0, gfp);
}

void pmm_free_pages(zx_page_t *page, uint32_t order) {
    if (!pmm_ready) panic("pmm_free_pages: PMM not initialised");
    if (!page) panic("pmm_free_pages: NULL page");

    // --- UAF / double-free detection ---
    if (page->flags & PF_POISON)
        panic("pmm_free_pages: double-free PFN %llu",
              (unsigned long long)page_to_pfn(page));
    if (page->flags & PF_BUDDY)
        panic("pmm_free_pages: freeing already-free PFN %llu",
              (unsigned long long)page_to_pfn(page));

    zone_id_t zid   = (zone_id_t)page->zone_id;
    pmm_zone_t *zone = &zones[zid];
    uint64_t pfn     = page_to_pfn(page);

    irqflags_t irqf;
    spin_lock_irqsave(&zone->lock, &irqf);

    // Coalesce with buddy.
    uint32_t ord = order;
    while (ord < MAX_ORDER) {
        uint64_t buddy_pfn  = pfn ^ (1ULL << ord);
        if (buddy_pfn >= max_pfn_global) break;
        zx_page_t *buddy = pfn_to_page(buddy_pfn);
        // Buddy must be free at this exact order and same zone.
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
    pmm_free_pages(page, 0);
}

// ---------------------------------------------------------------------------
// pmm_reserve_range()
// ---------------------------------------------------------------------------

void pmm_reserve_range(uint64_t phys_start, uint64_t phys_end) {
    if (!pmm_ready) return; // Called before init? ignore.

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

        // Find and remove from whichever order this block heads.
        // A reserved range may cut across multiple buddy blocks.
        // We look for a block at order 0 that contains this PFN.
        // If the PFN is in the middle of a larger block, we need to
        // split that block down to order 0 first.
        uint32_t ord = p->order;
        // Align pfn down to the block head for this order.
        uint64_t block_head = pfn & ~((1ULL << ord) - 1);

        if (free_area_remove(zone, ord, block_head)) {
            zone->free_pages -= (1ULL << ord);
            // Split the block and put back any sub-blocks that do NOT
            // overlap the reservation.
            while (ord > 0) {
                ord--;
                uint64_t lo = block_head;
                uint64_t hi = block_head + (1ULL << ord);
                // Determine which half contains our target pfn.
                if (pfn >= hi) {
                    // pfn is in the high half; put low half back.
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
            // block_head is now a single-page block containing pfn.
            // Mark it reserved.
            zx_page_t *rp = pfn_to_page(block_head);
            rp->flags  = PF_RESERVED;
            rp->order  = 0;
            atomic_set(&rp->refcount, 1);
        }
        spin_unlock_irqrestore(&zone->lock, f);
    }
}

// ---------------------------------------------------------------------------
// Statistics / accessors
// ---------------------------------------------------------------------------

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
