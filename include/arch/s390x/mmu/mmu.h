// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/mmu.h
//
/// @brief z/Architecture MMU: 5-level DAT page-table manipulation and TLB management.
///        Supports EDAT-1 (1 MB segment large pages) and EDAT-2 (2 GB region-third
///        large pages) with transparent fallback to 4 KB pages when hardware lacks
///        the required facility.  All operations are SMP-safe via per-table spinlocks.
///
#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/sync/spinlock.h>

/// ASCE designation type: R1 root (5-level paging).
#define Z_ASCE_DT_R1        0x0CULL
/// ASCE table-length: 2048 entries.
#define Z_ASCE_TL_2048      0x03ULL

/// Invalid bit (bit 58 BE = bit 5 LE) — shared by region and segment entries.
/// Linux: _REGION_ENTRY_INVALID = 0x20, _SEGMENT_ENTRY_INVALID = 0x20.
#define Z_I_BIT             0x20ULL

/// Region-table type field (bits 60-61 BE = bits 3:2 LE).
/// Linux: _REGION_ENTRY_TYPE_R1/R2/R3.
#define Z_TT_R1             0x0CULL   ///< TT=11 — R1 table entry.
#define Z_TT_R2             0x08ULL   ///< TT=10 — R2 table entry.
#define Z_TT_R3             0x04ULL   ///< TT=01 — R3 table entry.
#define Z_TT_SEG            0x00ULL   ///< TT=00 — Segment table entry.

/// Table-length field: all region/segment tables have 2048 entries (bits 62-63 BE).
/// Linux: _REGION_ENTRY_LENGTH = 0x03.
#define Z_TL_2048           0x03ULL

/// STE Format-Control bit (FC=1 → 1 MB large page, requires EDAT-1, STFLE bit 8).
#define Z_STE_FC            0x0400ULL
/// STE protection bit (read-only). Linux: _SEGMENT_ENTRY_PROTECT = 0x200.
#define Z_STE_PROTECT       0x0200ULL
/// Mask to extract the 1 MB-aligned frame address from a large STE.
/// Linux: _SEGMENT_ENTRY_ORIGIN_LARGE = ~0xfffffUL.
#define Z_STE_LARGE_ORIGIN_MASK  (~0xFFFFFULL)
/// Size of an EDAT-1 large page: 1 MB.
#define Z_EDAT1_PAGE_SIZE   (1ULL << 20)
/// Alignment mask for EDAT-1 large pages.
#define Z_EDAT1_PAGE_MASK   (~(Z_EDAT1_PAGE_SIZE - 1ULL))
/// Number of 4 KB pages in one EDAT-1 large page.
#define Z_EDAT1_NR_PAGES    256U
/// PMM buddy order for one EDAT-1 large page (2^8 = 256 pages = 1 MB).
#define Z_EDAT1_ORDER       8U

/// R3 Format-Control bit (FC=1 → 2 GB large page, requires EDAT-2, STFLE bit 78).
/// Linux: _REGION3_ENTRY_LARGE = 0x0400.
#define Z_R3E_FC            0x0400ULL
/// R3 protection bit. Linux: _REGION_ENTRY_PROTECT = 0x200.
#define Z_R3E_PROTECT       0x0200ULL
/// Mask to extract the 2 GB-aligned frame address from a large R3 entry.
/// Linux: _REGION3_ENTRY_ORIGIN_LARGE = ~0x7fffffffUL.
#define Z_R3E_LARGE_ORIGIN_MASK  (~0x7FFFFFFFULL)
/// Size of an EDAT-2 large page: 2 GB.
#define Z_EDAT2_PAGE_SIZE   (1ULL << 31)
/// Alignment mask for EDAT-2 large pages.
#define Z_EDAT2_PAGE_MASK   (~(Z_EDAT2_PAGE_SIZE - 1ULL))
/// Number of 4 KB pages in one EDAT-2 large page.
#define Z_EDAT2_NR_PAGES    (1U << 19)   ///< 524288 pages
/// PMM buddy order for one EDAT-2 large page (2^19 = 524288 pages = 2 GB).
#define Z_EDAT2_ORDER       19U

/// PTE Invalid bit (bit 53 BE = bit 10 LE). Linux: _PAGE_INVALID = 0x400.
#define Z_PTE_I             0x400ULL
/// PTE Protected (read-only, bit 55 BE = bit 8 LE). Linux: _PAGE_PROTECT = 0x200.
#define Z_PTE_P             0x200ULL

/// Number of entries in R1/R2/R3/segment tables.
#define Z_TABLE_ENTRIES     2048U
/// Number of entries in a page table.
#define Z_PT_ENTRIES        256U
/// PMM buddy order for a 16 KB region/segment table (2^2 = 4 pages = 16 KB).
#define Z_TABLE_ORDER       2U
/// PMM buddy order for a 4 KB page table (2^0 = 1 page = 4 KB).
#define Z_PT_ORDER          0U

/// Region-First index (bits 63:53 of the 64-bit virtual address).
static inline uint32_t va_rfx(uint64_t va) { return (uint32_t)((va >> 53) & 0x7FFUL); }
/// Region-Second index (bits 52:42).
static inline uint32_t va_rsx(uint64_t va) { return (uint32_t)((va >> 42) & 0x7FFUL); }
/// Region-Third index (bits 41:31).
static inline uint32_t va_rtx(uint64_t va) { return (uint32_t)((va >> 31) & 0x7FFUL); }
/// Segment index (bits 30:20).
static inline uint32_t va_sx(uint64_t va)  { return (uint32_t)((va >> 20) & 0x7FFUL); }
/// Page-table index (bits 19:12).
static inline uint32_t va_px(uint64_t va)  { return (uint32_t)((va >> 12) & 0xFFUL);  }
/// Byte offset within page (bits 11:0).
static inline uint32_t va_bx(uint64_t va)  { return (uint32_t)(va & 0xFFFUL);         }

/// @brief Handle to a z/Architecture 5-level paging structure.
///        The R1 table is 16 KB (order=2), 16 KB-aligned.
///        lock serialises all structural modifications to this page table
///        on SMP systems; readers that only walk without modifying may
///        proceed without the lock (entries are written atomically as
///        64-bit stores on z/Architecture).
typedef struct {
    spinlock_t lock;    ///< Per-table spinlock; held during map/unmap/alloc.
    uint64_t   asce;    ///< Value to load into CR1 (R1 origin | DT | TL).
    uint64_t   r1_phys; ///< Physical address of the R1 table.
} mmu_pgtbl_t;


/// @brief Initialize the kernel MMU subsystem.
///        Inherits the bootloader-built ASCE from CR1, detects EDAT-1/2.
///        Called once from vmm_init() with DAT already enabled.
void mmu_init(void);

/// @brief Return a pointer to the kernel's page-table handle.
mmu_pgtbl_t *mmu_kernel_pgtbl(void);

/// @brief Allocate and zero-initialise a new R1-rooted page-table hierarchy.
/// @return Handle with valid asce/r1_phys, or {0,0} on PMM failure.
mmu_pgtbl_t mmu_pgtbl_alloc(void);

/// @brief Free a page-table hierarchy.
///        Recursively releases all subordinate tables back to the PMM.
///        Does NOT free the physical frames that were mapped.
/// @param pgtbl  Handle returned by mmu_pgtbl_alloc().
void mmu_pgtbl_free(mmu_pgtbl_t pgtbl);

/// @brief Map a single 4 KB page.
///        If a valid PTE already exists it is first invalidated with IPTE.
///        Intermediate tables are allocated on demand from the PMM.
///        Acquires pgtbl->lock internally.
/// @param pgtbl  Page table handle.
/// @param va     Virtual address (must be 4 KB aligned).
/// @param pa     Physical address (must be 4 KB aligned).
/// @param prot   Bitmask of VM_READ | VM_WRITE | VM_EXEC.
/// @return 0 on success, -1 if a PMM allocation for a table failed.
int mmu_map_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot);

/// @brief Map a 1 MB large page using an EDAT-1 STE (FC=1).
///        va and pa must both be 1 MB aligned.
///        If EDAT-1 is not available, transparently falls back to
///        Z_EDAT1_NR_PAGES (256) individual 4 KB mmu_map_page() calls.
///        Acquires pgtbl->lock internally.
/// @return 0 on success, -1 on PMM failure.
int mmu_map_large_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot);

/// @brief Map a 2 GB large page using an EDAT-2 R3 entry (FC=1).
///        va and pa must both be 2 GB (Z_EDAT2_PAGE_SIZE) aligned.
///        If EDAT-2 is not available, transparently falls back to
///        2048 individual mmu_map_large_page() calls (which themselves
///        fall back to 4 KB if EDAT-1 is also absent).
///        Acquires pgtbl->lock internally.
/// @return 0 on success, -1 on PMM failure.
int mmu_map_huge_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot);

/// @brief Unmap a single 4 KB page, issuing IPTE for SMP coherency.
///        The PTE is written with Z_PTE_I set.  The backing frame is NOT freed.
///        Acquires pgtbl->lock internally.
/// @param pgtbl  Page table handle.
/// @param va     Virtual address (page-aligned) to unmap.
void mmu_unmap_page(mmu_pgtbl_t *pgtbl, uint64_t va);

/// @brief Translate a virtual address to its physical address.
///        Lock-free: safe to call concurrently with other readers.
/// @param pgtbl  Page table to walk.
/// @param va     Virtual address.
/// @return Physical address, or ~0ULL if unmapped.
uint64_t mmu_virt_to_phys(const mmu_pgtbl_t *pgtbl, uint64_t va);

/// @brief Query whether a virtual address is backed by a 1 MB EDAT-1 large page.
/// @param pgtbl  Page table to walk.
/// @param va     Virtual address (any alignment).
/// @return true if the STE for this VA has FC=1.
bool mmu_is_large_page(const mmu_pgtbl_t *pgtbl, uint64_t va);

/// @brief Query whether a virtual address is backed by a 2 GB EDAT-2 huge page.
/// @param pgtbl  Page table to walk.
/// @param va     Virtual address (any alignment).
/// @return true if the R3 entry for this VA has FC=1.
bool mmu_is_huge_page(const mmu_pgtbl_t *pgtbl, uint64_t va);

/// @brief Return true if EDAT-1 (STFLE bit 8) is active.
bool mmu_has_edat1(void);

/// @brief Return true if EDAT-2 (STFLE bit 78) is active.
bool mmu_has_edat2(void);

/// @brief Install a page-table handle into CR1 and issue PTLB.
///        Caller must have IRQs disabled.
/// @param pgtbl  Handle to activate.
void mmu_load_pgtbl(const mmu_pgtbl_t *pgtbl);

/// @brief Purge the entire local TLB (PTLB).
///        Only use during address-space teardown; prefer mmu_ipte()
///        for single-page invalidation in SMP kernels.
static inline void mmu_flush_tlb_local(void) {
    __asm__ volatile("ptlb" ::: "memory");
}

/// @brief Invalidate a single PTE using the IPTE instruction.
///        IPTE is a serialising, hardware-broadcast operation on z/Architecture:
///        the microcode propagates the invalidation to all CPUs that hold a
///        matching TLB entry — no software IPI shootdown is needed.
///        We pass 0 as the page-table origin because CR1 already holds the
///        correct ASCE and the hardware resolves the PTE internally.
/// @param va  Virtual address of the page whose PTE must be invalidated.
static inline void mmu_ipte(uint64_t va) {
    __asm__ volatile(
        "ipte %[zero], %[va]\n"
        :
        : [zero] "r"((uint64_t)0), [va] "r"(va)
        : "memory"
    );
}
