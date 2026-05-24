// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/dma_pool.c
//
/// @brief DMA pool — sub-page physically contiguous allocations from ZONE_DMA.

#include <zxfoundation/memory/dma_pool.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/sys/syschk.h>
#include <lib/string.h>

// ---------------------------------------------------------------------------
// Bucket descriptor
// ---------------------------------------------------------------------------

/// @brief A single DMA page sliced into fixed-size slots.
///        The free list is a singly-linked list of slot indices stored
///        in the first sizeof(uint16_t) bytes of each free slot.
///        This avoids any external metadata allocation.
typedef struct dma_bucket_page {
    struct dma_bucket_page *next;   ///< Next page in the bucket's page list.
    zx_page_t              *page;   ///< Owning PMM page (ZONE_DMA, PF_PINNED).
    uint16_t                free_head; ///< Index of first free slot (0xFFFF = none).
    uint16_t                free_count;
    uint16_t                slot_size; ///< Bytes per slot (power of two).
    uint16_t                nr_slots;  ///< Total slots in this page.
} dma_bucket_page_t;

typedef struct dma_bucket {
    spinlock_t        lock;
    dma_bucket_page_t *pages;   ///< Linked list of pages serving this bucket.
    uint16_t          slot_size;
} dma_bucket_t;

static dma_bucket_t buckets[DMA_POOL_NR_BUCKETS];
static bool         dma_pool_ready = false;

// ---------------------------------------------------------------------------
// Bucket page management
// ---------------------------------------------------------------------------

static dma_bucket_page_t *bucket_page_alloc(uint16_t slot_size) {
    // Allocate one ZONE_DMA page for the data.
    zx_page_t *pg = pmm_dma_alloc(0, ZX_GFP_ZERO);
    if (!pg) return nullptr;

    // Allocate the bucket_page_t descriptor from the SLAB/kmalloc.
    // This is safe because dma_pool_init is called after slab_init.
    dma_bucket_page_t *bp = (dma_bucket_page_t *)kmalloc(sizeof(dma_bucket_page_t), ZX_GFP_ZERO);
    if (!bp) {
        pmm_dma_free(pg, 0);
        return nullptr;
    }

    bp->page       = pg;
    bp->slot_size  = slot_size;
    bp->next       = nullptr;
    pg->flags |= PF_PINNED;

    // Whole page is used for slots. Alignment is guaranteed by PMM.
    uint8_t *base  = (uint8_t *)(uintptr_t)hhdm_phys_to_virt(pmm_page_to_phys(pg));
    bp->nr_slots   = (uint16_t)(PAGE_SIZE / slot_size);
    bp->free_count = bp->nr_slots;
    bp->free_head  = 0;

    // Build the free list: each free slot stores the index of the next.
    for (uint16_t i = 0; i < bp->nr_slots - 1; i++) {
        uint16_t *link = (uint16_t *)(base + (uintptr_t)i * slot_size);
        *link = i + 1;
    }
    // Last slot: sentinel.
    uint16_t *last = (uint16_t *)(base + (uintptr_t)(bp->nr_slots - 1) * slot_size);
    *last = 0xFFFFU;

    return bp;
}

// ---------------------------------------------------------------------------
// dma_pool_init
// ---------------------------------------------------------------------------

void dma_pool_init(void) {
    for (uint32_t i = 0; i < DMA_POOL_NR_BUCKETS; i++) {
        spin_lock_init(&buckets[i].lock);
        buckets[i].pages     = nullptr;
        buckets[i].slot_size = (uint16_t)(1U << (i + DMA_POOL_MIN_SHIFT));
    }
    dma_pool_ready = true;
    printk(ZX_INFO "dma_pool: initialized %u buckets [%u, %u] bytes\n",
           DMA_POOL_NR_BUCKETS, DMA_POOL_MIN_SIZE, DMA_POOL_MAX_SIZE);
}

// ---------------------------------------------------------------------------
// dma_pool_alloc
// ---------------------------------------------------------------------------

uint64_t dma_pool_alloc(size_t size, gfp_t gfp) {
    if (!size) return 0;

    // Sizes above the pool maximum go directly to pmm_dma_alloc.
    if (size > DMA_POOL_MAX_SIZE || !dma_pool_ready) {
        uint32_t order = 0;
        size_t s = size;
        while ((PAGE_SIZE << order) < s) order++;
        zx_page_t *pg = pmm_dma_alloc(order, gfp);
        if (!pg) return 0;
        return pmm_page_to_phys(pg);
    }

    // Round size up to the next power of two >= DMA_POOL_MIN_SIZE.
    uint32_t shift = DMA_POOL_MIN_SHIFT;
    while ((1U << shift) < size) shift++;
    if (shift > DMA_POOL_MAX_SHIFT) {
        zx_page_t *pg = pmm_dma_alloc(0, gfp);
        return pg ? pmm_page_to_phys(pg) : 0;
    }

    uint32_t bucket_idx = shift - DMA_POOL_MIN_SHIFT;
    dma_bucket_t *b = &buckets[bucket_idx];

    irqflags_t f;
    spin_lock_irqsave(&b->lock, &f);

    // Find a page with a free slot.
    dma_bucket_page_t *bp = b->pages;
    while (bp && bp->free_count == 0)
        bp = bp->next;

    if (!bp) {
        // No page with free slots — allocate a new one.
        // Drop the lock while calling into the PMM to avoid holding
        // b->lock across pmm_dma_alloc (which acquires zone->lock).
        spin_unlock_irqrestore(&b->lock, f);
        bp = bucket_page_alloc(b->slot_size);
        spin_lock_irqsave(&b->lock, &f);
        if (!bp) {
            spin_unlock_irqrestore(&b->lock, f);
            return 0;
        }
        bp->next  = b->pages;
        b->pages  = bp;
    }

    // Pop the first free slot.
    uint16_t idx   = bp->free_head;
    uint8_t *base  = (uint8_t *)(uintptr_t)hhdm_phys_to_virt(pmm_page_to_phys(bp->page));
    uint8_t *slot  = base + (uintptr_t)idx * b->slot_size;
    bp->free_head = *(uint16_t *)slot;
    bp->free_count--;

    spin_unlock_irqrestore(&b->lock, f);

    if (gfp & ZX_GFP_ZERO)
        memset(slot, 0, b->slot_size);

    // Return the physical address of the slot.
    uint64_t page_phys = pmm_page_to_phys(bp->page);
    uint64_t slot_off  = (uint64_t)(slot - (uint8_t *)(uintptr_t)
                          hhdm_phys_to_virt(page_phys));
    return page_phys + slot_off;
}

// ---------------------------------------------------------------------------
// dma_pool_free
// ---------------------------------------------------------------------------

void dma_pool_free(uint64_t phys, size_t size) {
    if (!phys || !size) return;

    if (size > DMA_POOL_MAX_SIZE || !dma_pool_ready) {
        uint32_t order = 0;
        size_t s = size;
        while ((PAGE_SIZE << order) < s) order++;
        pmm_dma_free(phys_to_page(phys), order);
        return;
    }

    uint32_t shift = DMA_POOL_MIN_SHIFT;
    while ((1U << shift) < size) shift++;
    if (shift > DMA_POOL_MAX_SHIFT) {
        pmm_dma_free(phys_to_page(phys), 0);
        return;
    }

    uint32_t bucket_idx = shift - DMA_POOL_MIN_SHIFT;
    dma_bucket_t *b = &buckets[bucket_idx];

    // Find the bucket_page_t that owns this physical address.
    uint64_t page_phys = phys & PAGE_MASK;

    irqflags_t f;
    spin_lock_irqsave(&b->lock, &f);

    dma_bucket_page_t *bp = b->pages;
    while (bp) {
        if (pmm_page_to_phys(bp->page) == page_phys) break;
        bp = bp->next;
    }

    if (!bp) {
        spin_unlock_irqrestore(&b->lock, f);
        zx_system_check(ZX_SYSCHK_CORE_INTERNAL_ERROR,
                        "dma_pool_free: phys 0x%llx not in any bucket page",
                        (unsigned long long)phys);
        return;
    }

    uint8_t *slot = (uint8_t *)(uintptr_t)hhdm_phys_to_virt(phys);
    uint8_t *base = (uint8_t *)(uintptr_t)hhdm_phys_to_virt(pmm_page_to_phys(bp->page));
    uint16_t idx  = (uint16_t)((slot - base) / b->slot_size);

    // Push the slot back onto the free list.
    *(uint16_t *)slot = bp->free_head;
    bp->free_head     = idx;
    bp->free_count++;

    spin_unlock_irqrestore(&b->lock, f);
}
