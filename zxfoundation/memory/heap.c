// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/heap.c
//
/// @brief Large-object kernel heap backed by the vmalloc region.
///
///        HEADER LAYOUT
///        =============
///        Every allocation prepends an 8-byte-aligned heap_hdr_t immediately
///        before the returned pointer:
///
///          [ heap_hdr_t | <padding to align> | user data ]
///          ^                                  ^
///          alloc_base (vmm_alloc return)      returned to caller
///
///        heap_hdr_t stores:
///          - HEAP_MAGIC: 64-bit canary — detects buffer-under-writes.
///          - total_size: the full vmm_alloc() byte count (including header).
///
///        kheap_free() walks back sizeof(heap_hdr_t) bytes from the user
///        pointer, verifies the magic, and calls vmm_free() with the base.
///
///        SIZING
///        ======
///        Internal size = ALIGN_UP(sizeof(heap_hdr_t) + size, PAGE_SIZE).
///        This wastes up to PAGE_SIZE-1 bytes at the end, which is acceptable
///        for objects > 8 KB.  A future slab-backed large-page allocator can
///        reclaim these tail bytes by using compound folios.
///
///        THREAD SAFETY
///        =============
///        All locking is delegated to vmm_alloc() / vmm_free().

#include <zxfoundation/memory/heap.h>
#include <zxfoundation/memory/vmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/zconfig.h>

// ---------------------------------------------------------------------------
// Magic canary — 8-byte value placed in the header preceding each allocation.
// If this is corrupted on free, a buffer-under-write occurred.
// ---------------------------------------------------------------------------

#define HEAP_MAGIC  0x48455850414C4C4DULL   /* ASCII "HEXPALM\0" */

typedef struct {
    uint64_t magic;       ///< Must equal HEAP_MAGIC.
    uint64_t total_size;  ///< Total bytes passed to vmm_alloc(), including header.
} heap_hdr_t;

_Static_assert(sizeof(heap_hdr_t) == 16, "heap_hdr_t must be 16 bytes");
_Static_assert(__alignof__(heap_hdr_t) <= 8, "heap_hdr_t over-aligned");

// ---------------------------------------------------------------------------
// kheap_alloc
// ---------------------------------------------------------------------------

void *kheap_alloc(size_t size) {
    if (!size) return nullptr;

    // Total internal size: header + user data, rounded up to page boundary.
    uint64_t total = (uint64_t)sizeof(heap_hdr_t) + (uint64_t)size;
    total = (total + PAGE_SIZE - 1) & PAGE_MASK;

    // Allocate from the vmalloc region.  VM_READ|VM_WRITE gives read/write
    // PTEs; ZX_GFP_NORMAL chooses ZONE_NORMAL frames.
    uint64_t base = vmm_alloc(total, VM_READ | VM_WRITE | VM_KERNEL, ZX_GFP_NORMAL);
    if (!base) return nullptr;

    heap_hdr_t *hdr = (heap_hdr_t *)(uintptr_t)base;
    hdr->magic      = HEAP_MAGIC;
    hdr->total_size = total;

    // The user pointer starts immediately after the header.
    return (void *)(uintptr_t)(base + sizeof(heap_hdr_t));
}

// ---------------------------------------------------------------------------
// kheap_free
// ---------------------------------------------------------------------------

void kheap_free(void *ptr) {
    if (!ptr) return;

    heap_hdr_t *hdr = (heap_hdr_t *)((uintptr_t)ptr - sizeof(heap_hdr_t));

    // Validate the magic canary before touching the allocation metadata.
    // A corrupted magic indicates a buffer-under-write or a wild pointer.
    if (hdr->magic != HEAP_MAGIC)
        panic("kheap_free: corrupted heap header at %p (magic=%016llx)",
              ptr, (unsigned long long)hdr->magic);

    uint64_t base = (uint64_t)(uintptr_t)hdr;
    hdr->magic    = 0; // Poison the header to catch use-after-free.
    vmm_free(base);
}

// ---------------------------------------------------------------------------
// kheap_zalloc
// ---------------------------------------------------------------------------

void *kheap_zalloc(size_t size) {
    void *p = kheap_alloc(size);
    if (!p) return nullptr;
    // Zero the user-visible region only; the header is already written.
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < size; i++) b[i] = 0;
    return p;
}
