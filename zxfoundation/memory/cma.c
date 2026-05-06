// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/cma.c
//
/// @brief Contiguous Memory Allocator — bitmap-based, boot-reserved regions.

#include <zxfoundation/memory/cma.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/zconfig.h>
#include <lib/string.h>
#include <lib/bitmap.h>

static cma_region_t regions[CMA_MAX_REGIONS];
static uint32_t     nr_regions = 0;

cma_region_t *cma_register(uint64_t base, uint64_t size, const char *name) {
    if (nr_regions >= CMA_MAX_REGIONS) {
        printk("cma: too many regions (max %u)\n", CMA_MAX_REGIONS);
        return nullptr;
    }
    if (!size || (size & (PAGE_SIZE - 1))) {
        printk("cma: size not page-aligned\n");
        return nullptr;
    }

    uint64_t nr_pages = size / PAGE_SIZE;
    // Bitmap storage: ceil(nr_pages / 64) uint64_t words, placed at the
    // start of the region itself (first bitmap_pages pages are reserved).
    uint64_t bitmap_words = (nr_pages + 63) / 64;
    uint64_t bitmap_bytes = bitmap_words * sizeof(uint64_t);
    uint64_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    if (bitmap_pages >= nr_pages) {
        printk("cma: region too small to hold its own bitmap\n");
        return nullptr;
    }

    cma_region_t *r = &regions[nr_regions++];
    spin_lock_init(&r->lock);
    r->base_pfn     = base / PAGE_SIZE;
    r->nr_pages     = nr_pages;
    r->name         = name;
    r->bitmap_words = bitmap_words;

    // The bitmap lives at the physical base of the region, accessed via HHDM.
    r->bitmap = (uint64_t *)(uintptr_t)hhdm_phys_to_virt(base);
    memset(r->bitmap, 0, bitmap_bytes);

    // Mark the bitmap pages themselves as allocated so they are never handed out.
    bitmap_set_range(r->bitmap, 0, bitmap_pages);

    printk("cma: registered '%s' base=0x%llx pages=%llu (bitmap=%llu pages)\n",
           name,
           (unsigned long long)base,
           (unsigned long long)nr_pages,
           (unsigned long long)bitmap_pages);
    return r;
}

/// @brief First-fit search for a free aligned run of 2^order pages.
///        Returns the page index within the region, or UINT64_MAX on failure.
static uint64_t cma_find_free(const cma_region_t *r, uint32_t order) {
    uint64_t count  = (uint64_t)1 << order;
    uint64_t align  = count; // natural alignment
    for (uint64_t i = 0; i + count <= r->nr_pages; i += align) {
        if (bitmap_range_free(r->bitmap, i, count))
            return i;
    }
    return UINT64_MAX;
}

zx_page_t *cma_alloc(cma_region_t *region, uint32_t order, gfp_t gfp) {
    uint64_t count = (uint64_t)1 << order;

    // If no specific region requested, try all in registration order.
    uint32_t start = 0, end = nr_regions;
    if (region) {
        start = (uint32_t)(region - regions);
        end   = start + 1;
    }

    for (uint32_t ri = start; ri < end; ri++) {
        cma_region_t *r = &regions[ri];

        irqflags_t f;
        spin_lock_irqsave(&r->lock, &f);

        uint64_t idx = cma_find_free(r, order);
        if (idx == UINT64_MAX) {
            spin_unlock_irqrestore(&r->lock, f);
            continue;
        }

        bitmap_set_range(r->bitmap, idx, count);
        spin_unlock_irqrestore(&r->lock, f);

        uint64_t pfn  = r->base_pfn + idx;
        zx_page_t *pg = pfn_to_page(pfn);

        // Initialise page descriptors for the block.
        pg->flags         = PF_HEAD;
        pg->compound_order = order;
        atomic_set(&pg->refcount, 1);
        for (uint64_t i = 1; i < count; i++) {
            zx_page_t *tail = pfn_to_page(pfn + i);
            tail->flags      = PF_TAIL;
            tail->tail_offset = (uint32_t)i;
        }

        if (gfp & ZX_GFP_ZERO) {
            uint8_t *va = (uint8_t *)(uintptr_t)
                hhdm_phys_to_virt(pmm_pfn_to_phys(pfn));
            uint64_t sz = count * PAGE_SIZE;
            for (uint64_t b = 0; b < sz; b++) va[b] = 0;
        }
        return pg;
    }
    return nullptr;
}

void cma_free(zx_page_t *page, uint32_t order) {
    if (!page) return;

    uint64_t pfn   = page_to_pfn(page);
    uint64_t count = (uint64_t)1 << order;

    for (uint32_t ri = 0; ri < nr_regions; ri++) {
        cma_region_t *r = &regions[ri];
        if (pfn < r->base_pfn || pfn >= r->base_pfn + r->nr_pages)
            continue;

        uint64_t idx = pfn - r->base_pfn;

        irqflags_t f;
        spin_lock_irqsave(&r->lock, &f);
        bitmap_clear_range(r->bitmap, idx, count);
        spin_unlock_irqrestore(&r->lock, f);

        // Clear compound page flags.
        page->flags &= ~PF_HEAD;
        for (uint64_t i = 1; i < count; i++) {
            zx_page_t *tail = pfn_to_page(pfn + i);
            tail->flags &= ~PF_TAIL;
        }
        atomic_set(&page->refcount, 0);
        return;
    }
    panic("cma_free: PFN %llu not in any CMA region", (unsigned long long)pfn);
}

void cma_dump(void) {
    for (uint32_t i = 0; i < nr_regions; i++) {
        cma_region_t *r = &regions[i];
        irqflags_t f;
        spin_lock_irqsave(&r->lock, &f);
        uint64_t used = 0;
        for (uint64_t w = 0; w < r->bitmap_words; w++) {
            /* Manual Kernighan popcount — no libgcc dependency. */
            uint64_t v = r->bitmap[w];
            while (v) { used++; v &= v - 1; }
        }
        uint64_t free_pages = r->nr_pages - used;
        spin_unlock_irqrestore(&r->lock, f);
        printk("cma: [%u] '%s' base_pfn=%llu pages=%llu free=%llu\n",
               i, r->name,
               (unsigned long long)r->base_pfn,
               (unsigned long long)r->nr_pages,
               (unsigned long long)free_pages);
    }
}

/// @brief Register a CMA region from the first suitable ZONE_NORMAL range.
///        64 MB, 2 MB-aligned, for EDAT-1/2 and large DMA buffers.
void cma_init(const zxfl_boot_protocol_t *boot) {
    if (!(boot->flags & ZXFL_FLAG_MEM_MAP)) return;

    const zxfl_mem_region_t *map =
        (const zxfl_mem_region_t *)(uintptr_t)boot->mem_map_addr;

    constexpr uint64_t cma_size  = 64UL * 1024 * 1024;

    for (uint32_t i = 0; i < boot->mem_map_count; i++) {
        if (map[i].type != ZXFL_MEM_USABLE) continue;

        uint64_t base = map[i].base;
        if (base < 16UL * 1024 * 1024) continue;

        constexpr uint64_t align     = 2UL  * 1024 * 1024;
        base = (base + align - 1) & ~(align - 1);

        if (map[i].length < cma_size * 2) continue;

        cma_register(base, cma_size, "cma0");
        return;
    }

}
