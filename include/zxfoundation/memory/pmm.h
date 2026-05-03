// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/pmm.h
//
/// @brief Physical Memory Manager (PMM) — page-granularity frame allocator.
///
///        DESIGN
///        ======
///        The PMM uses a dual-bitmap over the entire physical address space:
///          free_map  — bit set means the frame is available.
///          poison_map — bit set means the frame was freed and not yet
///                       reallocated; a set bit here on alloc is a
///                       use-after-free indicator.
///
///        Both bitmaps are stored in a statically-sized region carved out
///        of the first usable RAM region at init time.  The maximum
///        addressable physical memory is PMM_MAX_PHYS_PAGES frames.
///
///        LOCKING
///        =======
///        A single spinlock serializes all alloc/free operations.  This is
///        correct for a uniprocessor kernel and safe for SMP with low
///        allocation rates.  A per-zone lock or lock-free bitmap can be
///        added later without changing the API.
///
///        FRAME NUMBERING
///        ===============
///        Physical frame number (PFN) = physical_address >> PAGE_SHIFT.
///        All API functions work in PFNs.  Callers convert to/from byte
///        addresses using pmm_pfn_to_phys() / pmm_phys_to_pfn().

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/zconfig.h>
#include <arch/s390x/init/zxfl/zxfl.h>

#define PAGE_SHIFT      12
#define PAGE_SIZE       CONFIG_PAGE_SIZE
#define PAGE_MASK       (~(PAGE_SIZE - 1))

/// @brief Maximum physical pages the PMM can track (covers 4 GB).
///        Two bitmaps of this size = 2 * (4G/4K/8) = 2 * 128 KB = 256 KB of BSS.
///        This fits within the linker layout (kernel loads at 0x40000,
///        .zxfl_lock is at 0xB0000 — 448 KB of headroom).
#define PMM_MAX_PHYS_PAGES  (4UL * 1024 * 1024 * 1024 / PAGE_SIZE)

/// @brief Invalid PFN sentinel.
#define PMM_INVALID_PFN     (~(uint64_t)0)

static inline uint64_t pmm_phys_to_pfn(uint64_t phys) { return phys >> PAGE_SHIFT; }
static inline uint64_t pmm_pfn_to_phys(uint64_t pfn)  { return pfn  << PAGE_SHIFT; }

/// @brief PMM statistics snapshot.
typedef struct {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t reserved_pages;
} pmm_stats_t;

/// @brief Initialize the PMM from the ZXFL memory map.
///        Must be called once, early in boot, before any alloc/free.
/// @param boot  Validated ZXFL boot protocol pointer.
void pmm_init(const zxfl_boot_protocol_t *boot);

/// @brief Allocate one physical page.
/// @return Physical frame number, or PMM_INVALID_PFN on failure.
uint64_t pmm_alloc_page(void);

/// @brief Allocate n contiguous physical pages.
/// @return PFN of the first frame, or PMM_INVALID_PFN on failure.
uint64_t pmm_alloc_pages(uint64_t n);

/// @brief Free a single physical page.
/// @param pfn  Frame number returned by pmm_alloc_page().
void pmm_free_page(uint64_t pfn);

/// @brief Free n contiguous pages starting at pfn.
void pmm_free_pages(uint64_t pfn, uint64_t n);

/// @brief Mark a physical range as reserved (not available for allocation).
///        Used to protect MMIO regions, firmware tables, etc.
void pmm_reserve_range(uint64_t phys_start, uint64_t phys_end);

/// @brief Fill stats with a snapshot of current PMM state.
void pmm_get_stats(pmm_stats_t *out);

/// @brief Return the highest PFN seen in the memory map.
///        Valid after pmm_init().  Used by page_init() to size mem_map.
uint64_t pmm_get_max_pfn(void);
