// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/mmu.h
//
/// @brief z/Architecture MMU: page-table manipulation and TLB management.
///
///        PAGING HIERARCHY (PoP SA22-7832-13, §3)
///        =========================================
///          ASCE (CR1, DT=11) → R1[2048] → R2[2048] → R3[2048]
///                           → Seg[2048] → PT[256] → 4 KB page
///
///        ENTRY FORMATS
///        =============
///          Region entries (R1/R2/R3): 64-bit
///            [63:53] Table origin (right-shifted by 11 = 2 KB aligned)
///            [51:50] TT — Table-Type (11=R1, 10=R2, 01=R3, 00=seg)
///            [49:48] TL — Table length (11=2048 entries)
///            [32]    I  — Invalid
///
///          Segment Table Entry: 64-bit
///            [63:53] PT origin (2 KB aligned) OR large-page PFN
///            [54]    FC — Format Control (1=large 1 MB page via EDAT-1)
///            [32]    I  — Invalid
///
///          Page Table Entry: 64-bit
///            [63:53] Page-Frame Real Address (4 KB frame origin)
///            [55]    P  — Protected (read-only)
///            [53]    I  — Invalid  (Z_PTE_I)
///
///        SMP TLB COHERENCY
///        =================
///        After modifying a PTE that was previously valid, the kernel MUST
///        invalidate TLBs on all CPUs sharing the ASCE.  On z/Architecture
///        the IPTE instruction performs hardware-broadcast invalidation
///        atomically — no software IPI is required.  PTLB is reserved for
///        full address-space teardown on the local CPU only.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/zconfig.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/memory/vm_flags.h>

// ---------------------------------------------------------------------------
// Entry-level bit constants
// ---------------------------------------------------------------------------

/// ASCE designation type: R1 root (5-level paging).
#define Z_ASCE_DT_R1        0x0CULL
/// ASCE table-length: 2048 entries.
#define Z_ASCE_TL_2048      0x03ULL

/// Invalid bit (bit 58 BE = bit 5 LE) — shared by region, segment entries.
/// Per PoP SA22-7832, §3.11: bit 58 of region/segment table entries.
#define Z_I_BIT             0x20ULL

/// Region-table type field (bits 60-61 BE = bits 3:2 LE).
#define Z_TT_R1             0x0CULL   ///< TT=11 — R1 table entry.
#define Z_TT_R2             0x08ULL   ///< TT=10 — R2 table entry.
#define Z_TT_R3             0x04ULL   ///< TT=01 — R3 table entry.
#define Z_TT_SEG            0x00ULL   ///< TT=00 — Segment table entry.

/// Table-length field: all tables have 2048 entries (bits 62-63 BE = bits 1:0 LE).
#define Z_TL_2048           0x03ULL

/// Segment Table Entry — Format-Control (FC=1 → 1 MB large page, EDAT-1).
/// Bit 54 BE = bit 9 LE.
#define Z_STE_FC            0x200ULL

/// Page Table Entry — Invalid bit (bit 53 BE = bit 10 LE).
#define Z_PTE_I             0x400ULL
/// Page Table Entry — Protected (read-only, bit 55 BE = bit 8 LE).
#define Z_PTE_P             0x100ULL

// ---------------------------------------------------------------------------
// Virtual-address index extraction
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// mmu_pgtbl_t — opaque handle to a live paging hierarchy
// ---------------------------------------------------------------------------

/// @brief Handle to a z/Architecture 5-level paging structure.
///        The R1 table is 16 KB, 16 KB-aligned.  All subordinate tables
///        are 4 KB blocks allocated from the PMM.
typedef struct {
    uint64_t asce;      ///< Value to load into CR1 (R1 origin | DT | TL).
    uint64_t r1_phys;   ///< Physical address of the R1 table.
} mmu_pgtbl_t;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// @brief Initialize the kernel MMU subsystem.
///        Constructs the kernel-singleton page table (inheriting the
///        bootloader-built ASCE) and records kernel segment mappings.
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
/// @param pgtbl  Page table handle.
/// @param va     Virtual address (must be 4 KB aligned).
/// @param pa     Physical address (must be 4 KB aligned).
/// @param prot   Bitmask of VM_READ | VM_WRITE | VM_EXEC.
/// @return 0 on success, -1 if a PMM allocation for a table failed.
int mmu_map_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot);

/// @brief Map a 1 MB large page using an EDAT-1 STE (FC=1).
///        va and pa must both be 1 MB (0x100000) aligned.
///        Falls back transparently to 256× 4 KB mappings if the hardware
///        does not advertise EDAT-1 via the STFLE facility bit 78.
/// @return 0 on success, -1 on failure.
int mmu_map_large_page(mmu_pgtbl_t *pgtbl, uint64_t va, uint64_t pa, uint32_t prot);

/// @brief Unmap a single 4 KB page, issuing IPTE for SMP coherency.
///        The PTE is written with Z_PTE_I set.  The backing frame is NOT freed.
/// @param pgtbl  Page table handle.
/// @param va     Virtual address (page-aligned) to unmap.
void mmu_unmap_page(mmu_pgtbl_t *pgtbl, uint64_t va);

/// @brief Translate a virtual address to its physical address.
/// @param pgtbl  Page table to walk.
/// @param va     Virtual address.
/// @return Physical address, or ~0ULL if unmapped.
uint64_t mmu_virt_to_phys(const mmu_pgtbl_t *pgtbl, uint64_t va);

/// @brief Install a page-table handle into CR1 and issue PTLB.
///        Caller must have IRQs disabled.
/// @param pgtbl  Handle to activate.
void mmu_load_pgtbl(const mmu_pgtbl_t *pgtbl);

// ---------------------------------------------------------------------------
// TLB management inlines
// ---------------------------------------------------------------------------

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
///        The instruction takes the base of the *page table* (not the R1 root)
///        and the VA; we pass 0 as the base because CR1 is already loaded with
///        the correct ASCE and the hardware resolves the PTE internally.
/// @param va  Virtual address of the page whose PTE must be invalidated.
static inline void mmu_ipte(uint64_t va) {
    __asm__ volatile(
        "ipte %[zero], %[va]\n"
        :
        : [zero] "r"((uint64_t)0), [va] "r"(va)
        : "memory"
    );
}