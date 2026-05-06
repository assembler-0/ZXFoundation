// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/cma.h
//
/// @brief Contiguous Memory Allocator (CMA).
///
///        PURPOSE
///        =======
///        The buddy allocator caps contiguous allocations at MAX_ORDER (4 MB).
///        EDAT-2 2 GB large pages and large DMA buffers require physically
///        contiguous regions that exceed this limit.  CMA reserves one or more
///        contiguous physical ranges at boot time (before the buddy pool is
///        populated) and manages them with a per-region bitmap allocator.
///
///        DESIGN
///        ======
///        Each cma_region_t covers a power-of-two-aligned physical range.
///        Allocations are in units of 2^order pages (same convention as the
///        PMM).  A simple first-fit bitmap search is used; fragmentation is
///        acceptable because CMA regions are used for large, long-lived
///        allocations (EDAT-2 mappings, DMA coherent buffers).
///
///        THREAD SAFETY
///        =============
///        Each region has its own spinlock.  cma_alloc / cma_free are
///        SMP-safe.  cma_init() must be called before any AP is started.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>

/// Maximum number of CMA regions registered at boot.
#define CMA_MAX_REGIONS     4U

typedef struct cma_region {
    spinlock_t  lock;
    uint64_t    base_pfn;   ///< First PFN of the region.
    uint64_t    nr_pages;   ///< Total pages in the region (power of two).
    uint64_t   *bitmap;     ///< One bit per page: 1 = allocated, 0 = free.
    uint64_t    bitmap_words; ///< Number of uint64_t words in bitmap[].
    const char *name;       ///< Diagnostic label.
} cma_region_t;

/// @brief Register a physical range as a CMA region.
///        Must be called from pmm_init() before buddy pages are added,
///        so the range is excluded from the buddy pool.
///        The bitmap is carved from the region itself (first pages).
/// @param base   Physical base address (must be PAGE_SIZE aligned).
/// @param size   Byte length (must be a multiple of PAGE_SIZE).
/// @param name   Static diagnostic label.
/// @return Pointer to the registered region, or nullptr on failure.
cma_region_t *cma_register(uint64_t base, uint64_t size, const char *name);

/// @brief Allocate a 2^order contiguous block from a CMA region.
/// @param region  Target region (nullptr = search all regions).
/// @param order   Block size exponent (0 = 4 KB).
/// @param gfp     ZX_GFP_ZERO to zero the block before return.
/// @return Head zx_page_t *, or nullptr on failure.
zx_page_t *cma_alloc(cma_region_t *region, uint32_t order, gfp_t gfp);

/// @brief Free a block returned by cma_alloc().
/// @param page   Head page returned by cma_alloc().
/// @param order  Must match the order used at allocation time.
void cma_free(zx_page_t *page, uint32_t order);

/// @brief Print a one-line summary of all registered CMA regions.
void cma_dump(void);

/// @brief Register a CMA region from the boot memory map.
///        Called once from main.c after pmm_init().
/// @param boot  Validated ZXFL boot protocol.
void cma_init(const zxfl_boot_protocol_t *boot);
