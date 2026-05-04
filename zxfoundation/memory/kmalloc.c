// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/kmalloc.c
//
/// @brief General-purpose kernel allocator — power-of-2 slab dispatch.

#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/slab.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <lib/vsprintf.h>

#define KMALLOC_SHIFT_LOW   5    ///< 2^5  =  32 bytes — smallest class.
#define KMALLOC_SHIFT_HIGH  13   ///< 2^13 = 8192 bytes — largest class.
#define NUM_KMALLOC_CACHES  (KMALLOC_SHIFT_HIGH - KMALLOC_SHIFT_LOW + 1)

static kmem_cache_t *kmalloc_caches[NUM_KMALLOC_CACHES];

void kmalloc_init(void) {
    char buf[32];
    for (int i = 0; i < NUM_KMALLOC_CACHES; i++) {
        size_t sz = 1UL << (i + KMALLOC_SHIFT_LOW);
        snprintf(buf, sizeof(buf), "kmalloc-%zu", sz);
        kmalloc_caches[i] = kmem_cache_create(buf, sz, 0);
        if (!kmalloc_caches[i])
            panic("kmalloc_init: failed to create cache size %zu", sz);
    }
    vmm_notify_slab_ready();
}

static inline int get_cache_index(size_t size) {
    if (!size || size > (1UL << KMALLOC_SHIFT_HIGH)) return -1;
    if (size <= (1UL << KMALLOC_SHIFT_LOW)) return 0;

    // TODO: Compute ceil(log2(size)).
    size_t s   = size - 1;
    int    msb = 0;
    while (s) { s >>= 1; msb++; }

    int idx = msb - (int)KMALLOC_SHIFT_LOW;
    if (idx < 0) idx = 0;
    if (idx >= NUM_KMALLOC_CACHES) return -1;
    return idx;
}

void *kmalloc(size_t size) {
    int idx = get_cache_index(size);
    if (idx < 0) {
        printk("sys: kmalloc: size %zu out of range [32, 8192]", size);
        return nullptr;
    }
    return kmem_cache_alloc(kmalloc_caches[idx]);
}

void kfree(void *ptr) {
    if (!ptr) return;
    zx_page_t *page = virt_to_page(ptr);
    if (!(page->flags & PF_SLAB) || !page->slab_cache) {
        printk("sys: kfree: ptr %p is not a slab object", ptr);
        return;
    }
    kmem_cache_free(page->slab_cache, ptr);
}
