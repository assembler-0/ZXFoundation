// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/kmalloc.c
//
/// @brief General-purpose kernel allocator — power-of-2 slab dispatch.
///
///        SIZE CLASSES
///        ============
///        Allocations from 32 B to 8 KB are routed to a dedicated
///        kmem_cache for each power-of-two step.  Larger allocations
///        panic — callers needing > 8 KB should use vmm_alloc().
///
///        kfree() determines the owning cache from the slab_cache pointer
///        in the backing zx_page_t descriptor (set at slab allocation time).
///
///        THREAD SAFETY
///        =============
///        All safety is inherited from the slab layer: per-CPU magazine
///        fast paths are lock-free (IRQs disabled); slow paths use the
///        per-cache depot spinlock.  kmalloc/kfree themselves add no locks.

#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/slab.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/panic.h>
#include <lib/vsprintf.h>

// ---------------------------------------------------------------------------
// Size-class table
// ---------------------------------------------------------------------------

#define KMALLOC_SHIFT_LOW   5    ///< 2^5  =  32 bytes — smallest class.
#define KMALLOC_SHIFT_HIGH  13   ///< 2^13 = 8192 bytes — largest class.
#define NUM_KMALLOC_CACHES  (KMALLOC_SHIFT_HIGH - KMALLOC_SHIFT_LOW + 1)

static kmem_cache_t *kmalloc_caches[NUM_KMALLOC_CACHES];

// ---------------------------------------------------------------------------
// kmalloc_init
// ---------------------------------------------------------------------------

void kmalloc_init(void) {
    char buf[32];
    for (int i = 0; i < NUM_KMALLOC_CACHES; i++) {
        size_t sz = 1UL << (i + KMALLOC_SHIFT_LOW);
        snprintf(buf, sizeof(buf), "kmalloc-%zu", sz);
        kmalloc_caches[i] = kmem_cache_create(buf, sz, 0);
        if (!kmalloc_caches[i])
            panic("kmalloc_init: failed to create cache size %zu", sz);
    }
    // Signal the VMM that vmm_alloc() / vmm_free() can now use kmalloc
    // for VMA descriptor storage (transitions from static pool).
    vmm_notify_slab_ready();
}

// ---------------------------------------------------------------------------
// get_cache_index — map a byte size to a kmalloc_caches[] index
// ---------------------------------------------------------------------------

static inline int get_cache_index(size_t size) {
    if (!size || size > (1UL << KMALLOC_SHIFT_HIGH)) return -1;
    if (size <= (1UL << KMALLOC_SHIFT_LOW)) return 0;

    // Compute ceil(log2(size)).
    size_t s   = size - 1;
    int    msb = 0;
    while (s) { s >>= 1; msb++; }

    int idx = msb - (int)KMALLOC_SHIFT_LOW;
    if (idx < 0) idx = 0;
    if (idx >= NUM_KMALLOC_CACHES) return -1;
    return idx;
}

// ---------------------------------------------------------------------------
// kmalloc / kfree
// ---------------------------------------------------------------------------

void *kmalloc(size_t size) {
    int idx = get_cache_index(size);
    if (idx < 0)
        panic("kmalloc: size %zu out of range [32, 8192]", size);
    return kmem_cache_alloc(kmalloc_caches[idx]);
}

void kfree(void *ptr) {
    if (!ptr) return;
    // Recover the owning cache from the page descriptor.
    // virt_to_page() maps the HHDM virtual address → zx_page_t.
    zx_page_t *page = virt_to_page(ptr);
    if (!(page->flags & PF_SLAB) || !page->slab_cache)
        panic("kfree: ptr %p is not a slab object", ptr);
    kmem_cache_free(page->slab_cache, ptr);
}
