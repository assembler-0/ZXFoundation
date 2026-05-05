// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/kmalloc.h

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/memory/pmm.h>

/// @brief Initialize the global kmalloc caches.
void kmalloc_init(void);

/// @brief Allocate general-purpose kernel memory.
/// @param size  Requested size in bytes.
/// @param gfp   PMM allocation flags (ZX_GFP_*).
///              ZX_GFP_ZERO zeroes the returned object.
///              ZX_GFP_DMA  allocates from ZONE_DMA.
/// @return Pointer to allocated memory, or nullptr on failure.
void *kmalloc(size_t size, gfp_t gfp);

/// @brief Allocate zeroed kernel memory.  Equivalent to kmalloc(size, gfp | ZX_GFP_ZERO).
static inline void *kzalloc(size_t size, gfp_t gfp) {
    return kmalloc(size, gfp | ZX_GFP_ZERO);
}

/// @brief Free memory returned by kmalloc / kzalloc.
void kfree(void *ptr);
