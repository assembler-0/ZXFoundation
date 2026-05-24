// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/kmalloc.c
//
/// @brief General-purpose kernel heap.
///
///        SIZE CLASSES
///        ============
///        [32 B, 8 KB]  — slab magazine path (KMALLOC_SHIFT_LOW..KMALLOC_SHIFT_HIGH)
///        (8 KB, ∞)     — direct PMM allocation, tagged PF_KMALLOC on the head page
///
///        The 8 KB ceiling is deliberate.  Objects larger than two pages have
///        poor slab utilization (the magazine holds very few objects, cache
///        locality is lost) and are rare enough that the PMM round-trip cost
///        is irrelevant.  Linux uses the same boundary.
///
///        kfree() detects which path was used via PF_KMALLOC on the page
///        descriptor, so the caller never needs to know.

#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/slab.h>
#include <zxfoundation/memory/vmalloc.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>
#include <lib/string.h>
#include <lib/vsprintf.h>

/// Slab-backed size classes: 2^5 (32 B) through 2^13 (8 KB).
#define KMALLOC_SHIFT_LOW   5U
#define KMALLOC_SHIFT_HIGH  13U
#define NUM_KMALLOC_CACHES  (KMALLOC_SHIFT_HIGH - KMALLOC_SHIFT_LOW + 1U)

static kmem_cache_t *kmalloc_caches[NUM_KMALLOC_CACHES];

void kmalloc_init(void) {
    char buf[32];
    for (uint32_t i = 0; i < NUM_KMALLOC_CACHES; i++) {
        size_t sz = 1UL << (i + KMALLOC_SHIFT_LOW);
        snprintf(buf, sizeof(buf), "kmalloc-%zu", sz);
        kmalloc_caches[i] = kmem_cache_create(buf, sz, 0);
        if (!kmalloc_caches[i])
            zx_system_check(ZX_SYSCHK_MEM_OOM,
                            "kmalloc_init: failed to create cache size %zu", sz);
    }
    vmm_notify_slab_ready();
}

/// @brief Map a requested size to a slab cache index.
///        Returns -1 if the size exceeds the slab ceiling (use PMM directly).
static inline int get_cache_index(size_t size) {
    if (!size || size > KMALLOC_SLAB_MAX) return -1;
    if (size <= (1UL << KMALLOC_SHIFT_LOW)) return 0;
    // Round up to next power of two, find its shift.
    size_t s = size - 1;
    int msb = 0;
    while (s) { s >>= 1; msb++; }
    int idx = msb - (int)KMALLOC_SHIFT_LOW;
    if (idx < 0) idx = 0;
    if (idx >= (int)NUM_KMALLOC_CACHES) return -1;
    return idx;
}

void *kmalloc(size_t size, gfp_t gfp) {
    if (!size) return nullptr;

    int idx = get_cache_index(size);
    if (idx >= 0) {
        void *ptr = kmem_cache_alloc(kmalloc_caches[idx], gfp);
        if (ptr && (gfp & ZX_GFP_ZERO))
            memset(ptr, 0, size);
        return ptr;
    }

    // Large allocation: go directly to the PMM.
    // Compute the order needed to cover 'size' bytes.
    uint32_t order = 0;
    while ((PAGE_SIZE << order) < size) order++;

    zx_page_t *page = pmm_alloc_pages(order, gfp);
    if (!page) return nullptr;

    // Tag the head page so kfree knows to call pmm_free_pages.
    page->flags |= PF_KMALLOC;
    page->order  = (uint8_t)order;

    void *ptr = (void *)(uintptr_t)hhdm_phys_to_virt(pmm_page_to_phys(page));
    if (gfp & ZX_GFP_ZERO)
        memset(ptr, 0, PAGE_SIZE << order);
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;

    zx_page_t *page = virt_to_page(ptr);

    if (page->flags & PF_KMALLOC) {
        // Large allocation — free the PMM block.
        uint32_t order = page->order;
        page->flags &= ~PF_KMALLOC;
        pmm_free_pages(page, order);
        return;
    }

    if (!(page->flags & PF_SLAB) || !page->slab_cache) {
        printk(ZX_WARN "kfree: ptr %p is not a slab or kmalloc object\n", ptr);
        return;
    }
    kmem_cache_free(page->slab_cache, ptr);
}

void *kvmalloc(size_t size, gfp_t gfp) {
    if (size <= KMALLOC_MAX_SIZE) {
        void *p = kmalloc(size, gfp);
        if (p) return p;
    }
    if (gfp & ZX_GFP_DMA) return nullptr;

    void *p = vmalloc(size);
    if (p && (gfp & ZX_GFP_ZERO)) {
        uint8_t *b = (uint8_t *)p;
        for (size_t i = 0; i < size; i++) b[i] = 0;
    }
    return p;
}

void kvfree(void *ptr) {
    if (!ptr) return;
    if (is_vmalloc_addr(ptr))
        vfree(ptr);
    else
        kfree(ptr);
}
