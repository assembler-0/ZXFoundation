// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/dma_pool.h
//
/// @brief DMA pool — sub-page physically contiguous allocations from ZONE_DMA.
///
///        DESIGN
///        ======
///        The channel subsystem (CSS) requires physically contiguous buffers
///        below 16 MB (31-bit CDA constraint).  Most CCW channel programs
///        use small buffers (< 4 KB) — allocating a full page per buffer
///        wastes ZONE_DMA, which is only 16 MB.
///
///        The DMA pool maintains a set of slab-like buckets, each backed by
///        a single ZONE_DMA page.  Each bucket serves allocations of a fixed
///        power-of-two size.  Buckets are organized as a free-list of
///        physically contiguous slots within the page.
///
///        ALIGNMENT
///        =========
///        All allocations are naturally aligned to their size.  A 64-byte
///        allocation is 64-byte aligned.  This satisfies the CCW doubleword
///        alignment requirement for all practical buffer sizes.
///
///        INITIALIZATION
///        ==============
///        dma_pool_init() must be called after slab_init().  Before that,
///        pmm_dma_pool_alloc() falls back to pmm_dma_alloc(0, gfp) (full page).
///
///        THREAD SAFETY
///        =============
///        Each bucket has its own spinlock.  SMP-safe.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/memory/pmm.h>

/// Minimum allocation size: 64 bytes (CCW doubleword alignment).
#define DMA_POOL_MIN_SHIFT  6U
/// Maximum allocation size: 2048 bytes (half a page).
/// Larger allocations use pmm_dma_alloc() directly.
#define DMA_POOL_MAX_SHIFT  11U
#define DMA_POOL_MIN_SIZE   (1U << DMA_POOL_MIN_SHIFT)
#define DMA_POOL_MAX_SIZE   (1U << DMA_POOL_MAX_SHIFT)
#define DMA_POOL_NR_BUCKETS (DMA_POOL_MAX_SHIFT - DMA_POOL_MIN_SHIFT + 1)

/// @brief Initialize the DMA pool.
///        Must be called after slab_init() and pmm_init().
void dma_pool_init(void);

/// @brief Allocate @size bytes from the DMA pool.
///        Returns a physical address suitable for use as a CCW CDA.
///        The allocation is naturally aligned to the next power of two >= size.
///        Falls back to pmm_dma_alloc(0, gfp) for sizes > DMA_POOL_MAX_SIZE.
/// @param size  Requested size in bytes.
/// @param gfp   Allocation flags (ZX_GFP_ZERO supported).
/// @return Physical address, or 0 on failure.
uint64_t dma_pool_alloc(size_t size, gfp_t gfp);

/// @brief Return a DMA pool allocation.
/// @param phys  Physical address returned by dma_pool_alloc().
/// @param size  Must match the size passed to dma_pool_alloc().
void dma_pool_free(uint64_t phys, size_t size);

/// @brief Return the virtual (HHDM) address for a DMA physical address.
///        Convenience wrapper for drivers that need to write to the buffer.
static inline void *dma_phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)hhdm_phys_to_virt(phys);
}
