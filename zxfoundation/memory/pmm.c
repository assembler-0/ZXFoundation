// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/pmm.c

#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/spinlock.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <lib/bitmap.h>

#define PMM_BITMAP_WORDS    BITMAP_WORDS(PMM_MAX_PHYS_PAGES)

// free_map:   bit set → frame is available for allocation.
// poison_map: bit set → frame was freed and not yet reallocated (UAF detector).
static uint64_t free_map  [PMM_BITMAP_WORDS];
static uint64_t poison_map[PMM_BITMAP_WORDS];

static spinlock_t pmm_lock       = SPINLOCK_INIT;
static uint64_t   total_pages    = 0;
static uint64_t   free_pages_cnt = 0;
static uint64_t   max_pfn        = 0;
static bool       pmm_ready      = false;

void pmm_init(const zxfl_boot_protocol_t *boot) {
    if (!boot)
        panic("pmm_init: NULL boot protocol");

    bitmap_zero(free_map,   PMM_BITMAP_WORDS);
    bitmap_zero(poison_map, PMM_BITMAP_WORDS);
    total_pages = free_pages_cnt = max_pfn = 0;

    if (!(boot->flags & ZXFL_FLAG_MEM_MAP) || !boot->mem_map_count)
        panic("pmm_init: no memory map in boot protocol");

    const zxfl_mem_region_t *map =
        (const zxfl_mem_region_t *)(uintptr_t)boot->mem_map_addr;

    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        const zxfl_mem_region_t *r = &map[i];
        uint64_t pfn_start = pmm_phys_to_pfn(r->base);
        uint64_t pfn_end   = pmm_phys_to_pfn(r->base + r->length);
        if (pfn_end > PMM_MAX_PHYS_PAGES) pfn_end = PMM_MAX_PHYS_PAGES;
        if (pfn_end > max_pfn) max_pfn = pfn_end;

        if (r->type == ZXFL_MEM_USABLE) {
            bitmap_set_range(free_map, pfn_start, pfn_end - pfn_start);
            free_pages_cnt += pfn_end - pfn_start;
        }
        total_pages += pfn_end - pfn_start;
    }

    // Reserve low memory (lowcore + kernel image).
    uint64_t reserve_end = pmm_phys_to_pfn(boot->kernel_phys_end + PAGE_SIZE - 1);
    if (reserve_end > PMM_MAX_PHYS_PAGES) reserve_end = PMM_MAX_PHYS_PAGES;
    for (uint64_t pfn = 0; pfn < reserve_end; pfn++)
        if (bitmap_test_and_clear(free_map, pfn))
            free_pages_cnt--;

    pmm_ready = true;
    printk("pmm: %llu MB total, %llu MB free\n",
           (unsigned long long)(total_pages    * PAGE_SIZE / (1024 * 1024)),
           (unsigned long long)(free_pages_cnt * PAGE_SIZE / (1024 * 1024)));
}

uint64_t pmm_alloc_page(void) { return pmm_alloc_pages(1); }

uint64_t pmm_alloc_pages(uint64_t n) {
    if (!pmm_ready) panic("pmm_alloc_pages: PMM not initialized");
    if (!n) return PMM_INVALID_PFN;

    irqflags_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    uint64_t pfn = bitmap_find_next_set_run(free_map, max_pfn, 0, n);
    if (pfn >= max_pfn) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return PMM_INVALID_PFN;
    }
    bitmap_clear_range(free_map,   pfn, n);
    bitmap_clear_range(poison_map, pfn, n);
    free_pages_cnt -= n;

    spin_unlock_irqrestore(&pmm_lock, flags);
    return pfn;
}

void pmm_free_page(uint64_t pfn) { pmm_free_pages(pfn, 1); }

void pmm_free_pages(uint64_t pfn, uint64_t n) {
    if (!pmm_ready) panic("pmm_free_pages: PMM not initialized");
    if (pfn + n > PMM_MAX_PHYS_PAGES)
        panic("pmm_free_pages: PFN %llu+%llu out of range",
              (unsigned long long)pfn, (unsigned long long)n);

    irqflags_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    for (uint64_t i = 0; i < n; i++) {
        if (bitmap_test(poison_map, pfn + i))
            panic("pmm_free_pages: double-free of PFN %llu",
                  (unsigned long long)(pfn + i));
        if (bitmap_test(free_map, pfn + i))
            panic("pmm_free_pages: freeing already-free PFN %llu",
                  (unsigned long long)(pfn + i));
        bitmap_set(free_map,   pfn + i);
        bitmap_set(poison_map, pfn + i);
    }
    free_pages_cnt += n;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_reserve_range(uint64_t phys_start, uint64_t phys_end) {
    uint64_t pfn_start = pmm_phys_to_pfn(phys_start);
    uint64_t pfn_end   = pmm_phys_to_pfn(phys_end + PAGE_SIZE - 1);
    if (pfn_end > PMM_MAX_PHYS_PAGES) pfn_end = PMM_MAX_PHYS_PAGES;

    irqflags_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    for (uint64_t pfn = pfn_start; pfn < pfn_end; pfn++)
        if (bitmap_test_and_clear(free_map, pfn))
            free_pages_cnt--;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_get_stats(pmm_stats_t *out) {
    irqflags_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    out->total_pages    = total_pages;
    out->free_pages     = free_pages_cnt;
    out->reserved_pages = total_pages - free_pages_cnt;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_get_max_pfn(void) { return max_pfn; }