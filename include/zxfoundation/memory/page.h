// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/page.h
//
/// @brief Core page structures: zx_page and zx_folio.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/zconfig.h>

struct kmem_cache;

/// @brief Represents a single physical page frame (4KB).
///        Every valid PFN has a corresponding zx_page_t in the mem_map.
typedef struct zx_page {
    uint32_t flags;
    int32_t  refcount;

    union {
        struct {
            struct kmem_cache *slab_cache;
        };
    };
} zx_page_t;

extern zx_page_t *zx_mem_map;

#define PAGE_SHIFT 12

static inline zx_page_t *pfn_to_page(uint64_t pfn) {
    return &zx_mem_map[pfn];
}

static inline uint64_t page_to_pfn(const zx_page_t *page) {
    return (uint64_t)(page - zx_mem_map);
}

static inline zx_page_t *phys_to_page(uint64_t phys) {
    return pfn_to_page(phys >> PAGE_SHIFT);
}

static inline zx_page_t *virt_to_page(const void *virt) {
    uint64_t phys = hhdm_virt_to_phys((uint64_t)(uintptr_t)virt);
    return phys_to_page(phys);
}
