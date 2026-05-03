// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/heap.h
//
/// @brief Large-object kernel heap — vmalloc-backed allocations > 8 KB.
///
///        OVERVIEW
///        ========
///        kmalloc() handles objects up to 8 KB via power-of-2 slab caches.
///        For anything larger (kernel buffers, DMA descriptors, module images)
///        the heap layer allocates a virtual region from the vmalloc arena,
///        backed by individual PMM pages.
///
///        All allocations are 8-byte aligned.  A hidden heap_hdr_t is placed
///        at the base of each region so kheap_free() can determine the size
///        without an external lookup.
///
///        THREAD SAFETY
///        =============
///        kheap_alloc() / kheap_free() are fully SMP-safe: they call into
///        vmm_alloc() / vmm_free() which carry their own spinlocks.

#pragma once

#include <zxfoundation/types.h>

/// @brief Allocate 'size' bytes from the large-object kernel heap.
///        Returns a kernel virtual address (HHDM or vmalloc region).
///        The allocation is page-granular internally; the returned pointer
///        is aligned to at least 8 bytes.
/// @param size  Requested byte count (must be > 0).
/// @return Kernel virtual address, or NULL on failure.
void *kheap_alloc(size_t size);

/// @brief Free a pointer returned by kheap_alloc().
/// @param ptr  Must not be NULL; must be the exact pointer returned.
void kheap_free(void *ptr);

/// @brief Allocate and zero-fill 'size' bytes.
/// @return Kernel virtual address, or NULL on failure.
void *kheap_zalloc(size_t size);
