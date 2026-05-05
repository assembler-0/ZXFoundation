// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/slab.h
//
/// @brief Magazine-depot slab allocator.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/memory/pmm.h>

typedef struct kmem_cache kmem_cache_t;

/// @brief Initialise the bootstrap slab caches.
///        Must be called after pmm_init().  Populates the internal
///        cache_cache and mag_cache and seeds their first slab.
void slab_init(void);

/// @brief Signal to the VMM that kmalloc is now available.
///        Called at the end of kmalloc_init() so the VMM can switch from its
///        early static VMA pool to the dynamic allocator.
void vmm_notify_slab_ready(void);

/// @brief Create a named typed-object cache.
/// @param name         Static string for diagnostics.
/// @param size         Object size in bytes (internally padded to 8-byte boundary).
/// @param storage_key  s390x storage key for all slab pages (0 = default kernel).
/// @return New cache handle, or nullptr on OOM.
kmem_cache_t *kmem_cache_create(const char *name, size_t size, uint8_t storage_key);

/// @brief Allocate one object from the cache.
/// @param gfp  PMM flags used if a new slab page must be allocated.
/// @return Pointer to the object, or nullptr on OOM.
void *kmem_cache_alloc(kmem_cache_t *cache, gfp_t gfp);

/// @brief Return an object to its cache.
///        The pointer MUST have been returned by kmem_cache_alloc() on this cache.
void kmem_cache_free(kmem_cache_t *cache, void *obj);

/// @brief Destroy a cache, freeing all backing slab pages.
///        All objects must have been returned before calling this.
void kmem_cache_destroy(kmem_cache_t *cache);
