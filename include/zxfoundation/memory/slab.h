// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/slab.h

#pragma once

#include <zxfoundation/types.h>

typedef struct kmem_cache kmem_cache_t;

/// @brief Initialize the Slab system.
void slab_init(void);

/// @brief Create a new typed cache.
kmem_cache_t* kmem_cache_create(const char *name, size_t size, uint8_t storage_key);

/// @brief Allocate an object from the cache.
void* kmem_cache_alloc(kmem_cache_t *cache);

/// @brief Free an object back to the cache.
void kmem_cache_free(kmem_cache_t *cache, void *obj);
