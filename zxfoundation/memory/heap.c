// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/heap.c
//
/// @brief Large-object kernel heap backed by the vmalloc region.

#include <zxfoundation/memory/heap.h>
#include <zxfoundation/memory/vmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/zconfig.h>

#define HEAP_MAGIC  0x48455850414C4C4DULL   /* ASCII "HEXPALM\0" */

typedef struct {
    uint64_t magic;       ///< Must equal HEAP_MAGIC.
    uint64_t total_size;  ///< Total bytes passed to vmm_alloc(), including header.
} heap_hdr_t;

_Static_assert(sizeof(heap_hdr_t) == 16, "heap_hdr_t must be 16 bytes");
_Static_assert(__alignof__(heap_hdr_t) <= 8, "heap_hdr_t over-aligned");

void *kheap_alloc(size_t size) {
    if (!size) return nullptr;

    uint64_t total = (uint64_t)sizeof(heap_hdr_t) + (uint64_t)size;
    total = (total + PAGE_SIZE - 1) & PAGE_MASK;

    uint64_t base = vmm_alloc(total, VM_READ | VM_WRITE | VM_KERNEL, ZX_GFP_NORMAL);
    if (!base) return nullptr;

    heap_hdr_t *hdr = (heap_hdr_t *)(uintptr_t)base;
    hdr->magic      = HEAP_MAGIC;
    hdr->total_size = total;

    return (void *)(uintptr_t)(base + sizeof(heap_hdr_t));
}

void kheap_free(void *ptr) {
    if (!ptr) return;

    heap_hdr_t *hdr = (heap_hdr_t *)((uintptr_t)ptr - sizeof(heap_hdr_t));

    if (hdr->magic != HEAP_MAGIC)
        panic("kheap_free: corrupted heap header at %p (magic=%016llx)",
              ptr, (unsigned long long)hdr->magic);

    uint64_t base = (uint64_t)(uintptr_t)hdr;
    hdr->magic    = 0; // Poison the header to catch use-after-free.
    vmm_free(base);
}

void *kheap_zalloc(size_t size) {
    void *p = kheap_alloc(size);
    if (!p) return nullptr;
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < size; i++) b[i] = 0;
    return p;
}
