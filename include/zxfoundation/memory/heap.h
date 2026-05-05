// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/heap.h
//
/// @brief Large-object kernel heap — vmalloc-backed allocations > 8 KB.

#pragma once

#include <zxfoundation/types.h>

/// @brief Allocate 'size' bytes from the large-object kernel heap.
///        Returns a kernel virtual address (HHDM or vmalloc region).
///        The allocation is page-granular internally; the returned pointer
///        is aligned to at least 8 bytes.
/// @param size  Requested byte count (must be > 0).
/// @return Kernel virtual address, or nullptr on failure.
void *kheap_alloc(size_t size);

/// @brief Free a pointer returned by kheap_alloc().
/// @param ptr  Must not be nullptr; must be the exact pointer returned.
void kheap_free(void *ptr);

/// @brief Allocate and zero-fill 'size' bytes.
/// @return Kernel virtual address, or nullptr on failure.
void *kheap_zalloc(size_t size);
