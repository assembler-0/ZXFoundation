// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/kmalloc.h

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/memory/pmm.h>

/// @brief Initialize the global kmalloc caches.
void kmalloc_init(void);

/// @brief Allocate general-purpose kernel memory (slab-backed, ≤ 128 KB).
///        For sizes > 128 KB use vmalloc() or kvmalloc().
/// @param size  Requested size in bytes.
/// @param gfp   PMM allocation flags (ZX_GFP_*).
/// @return Pointer to allocated memory, or nullptr on failure.
void *kmalloc(size_t size, gfp_t gfp);

/// @brief Allocate zeroed kernel memory.
static inline void *kzalloc(size_t size, gfp_t gfp) {
    return kmalloc(size, gfp | ZX_GFP_ZERO);
}

/// @brief Free memory returned by kmalloc / kzalloc.
///        Do NOT pass vmalloc() pointers here; use kvfree() for unknown origin.
void kfree(void *ptr);

/// @brief Allocate kernel memory of any size.
///        Uses kmalloc for size ≤ KMALLOC_MAX_SIZE, vmalloc for larger.
///        The returned pointer must be freed with kvfree(), not kfree().
/// @param size  Requested byte count.
/// @param gfp   PMM allocation flags.
/// @return Pointer to allocated memory, or nullptr on failure.
void *kvmalloc(size_t size, gfp_t gfp);

/// @brief Allocate and zero-fill kernel memory of any size.
static inline void *kvzalloc(size_t size, gfp_t gfp) {
    return kvmalloc(size, gfp | ZX_GFP_ZERO);
}

/// @brief Free memory returned by kvmalloc / kvzalloc.
///        Correctly dispatches to kfree() or vfree() based on origin.
void kvfree(void *ptr);

/// @brief Maximum size serviced by kmalloc (slab path).
#define KMALLOC_MAX_SIZE    (1UL << 17)   ///< 128 KB
