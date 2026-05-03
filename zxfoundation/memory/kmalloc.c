// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/kmalloc.c

#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/slab.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/panic.h>
#include <lib/vsprintf.h>

#define KMALLOC_SHIFT_LOW   5   // 32 bytes
#define KMALLOC_SHIFT_HIGH  13  // 8192 bytes
#define NUM_KMALLOC_CACHES  (KMALLOC_SHIFT_HIGH - KMALLOC_SHIFT_LOW + 1)

static kmem_cache_t *kmalloc_caches[NUM_KMALLOC_CACHES];

void kmalloc_init(void) {
    char kmalloc_name[32] = {0};
    for (int i = 0; i < NUM_KMALLOC_CACHES; i++) {
        size_t size = 1UL << (i + KMALLOC_SHIFT_LOW);
        snprintf(kmalloc_name, sizeof(kmalloc_name), "kmalloc-%zu", size);
        kmalloc_caches[i] = kmem_cache_create(kmalloc_name, size, 0);
        if (!kmalloc_caches[i]) {
            panic("kmalloc_init: failed to create cache for size %zu", size);
        }
    }
}

static inline int get_cache_index(size_t size) {
    if (size == 0) return -1;
    if (size <= 32) return 0;
    
    size_t s = size - 1;
    int msb = 0;
    while (s > 0) {
        s >>= 1;
        msb++;
    }
    
    int idx = msb - KMALLOC_SHIFT_LOW;
    if (idx < 0) idx = 0;
    return idx;
}

void *kmalloc(size_t size) {
    int idx = get_cache_index(size);
    if (idx < 0 || idx >= NUM_KMALLOC_CACHES) {
        panic("kmalloc: size %zu exceeds max slab size", size);
    }
    return kmem_cache_alloc(kmalloc_caches[idx]);
}

void kfree(void *ptr) {
    if (!ptr) return;
    zx_page_t *page = virt_to_page(ptr);
    if (!page->slab_cache) panic("kfree: pointer not from slab");
    kmem_cache_free(page->slab_cache, ptr);
}
