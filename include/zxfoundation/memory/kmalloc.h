// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/kmalloc.h

#pragma once

#include <zxfoundation/types.h>

/// @brief Initialize the global kmalloc caches.
///        Must be called after slab_init().
void kmalloc_init(void);

/// @brief Allocate general-purpose kernel memory.
/// @param size  The requested size in bytes.
/// @return Pointer to the allocated memory, or nullptr on failure.
void *kmalloc(size_t size);

/// @brief Free general-purpose kernel memory.
/// @param ptr  Pointer returned by kmalloc.
void kfree(void *ptr);
