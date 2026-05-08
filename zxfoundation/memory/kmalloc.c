// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/kmalloc.c

#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/slab.h>
#include <zxfoundation/memory/vmalloc.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>
#include <lib/string.h>
#include <lib/vsprintf.h>

#define KMALLOC_SHIFT_LOW   5    ///< 2^5  =    32 bytes
#define KMALLOC_SHIFT_HIGH  17   ///< 2^17 = 131072 bytes (128 KB)
#define NUM_KMALLOC_CACHES  (KMALLOC_SHIFT_HIGH - KMALLOC_SHIFT_LOW + 1)

static kmem_cache_t *kmalloc_caches[NUM_KMALLOC_CACHES];

void kmalloc_init(void) {
    char buf[32];
    for (int i = 0; i < NUM_KMALLOC_CACHES; i++) {
        size_t sz = 1UL << (i + KMALLOC_SHIFT_LOW);
        snprintf(buf, sizeof(buf), "kmalloc-%zu", sz);
        kmalloc_caches[i] = kmem_cache_create(buf, sz, 0);
        if (!kmalloc_caches[i])
            zx_system_check(ZX_SYSCHK_MEM_OOM, "kmalloc_init: failed to create cache size %zu", sz);
    }
    vmm_notify_slab_ready();
}

static inline int get_cache_index(size_t size) {
    if (!size || size > (1UL << KMALLOC_SHIFT_HIGH)) return -1;
    if (size <= (1UL << KMALLOC_SHIFT_LOW)) return 0;
    size_t s = size - 1;
    int msb = 0;
    while (s) { s >>= 1; msb++; }
    int idx = msb - (int)KMALLOC_SHIFT_LOW;
    if (idx < 0) idx = 0;
    if (idx >= NUM_KMALLOC_CACHES) return -1;
    return idx;
}

void *kmalloc(size_t size, gfp_t gfp) {
    int idx = get_cache_index(size);
    if (idx < 0) {
        printk("kmalloc: size %zu out of range [32, 131072]\n", size);
        return nullptr;
    }
    void *ptr = kmem_cache_alloc(kmalloc_caches[idx], gfp);
    if (ptr && (gfp & ZX_GFP_ZERO))
        memset(ptr, 0, size);
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;
    zx_page_t *page = virt_to_page(ptr);
    if (!(page->flags & PF_SLAB) || !page->slab_cache) {
        printk("kfree: ptr %p is not a slab object\n", ptr);
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
