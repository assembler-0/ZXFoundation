// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/zxfl.h
//
/// @brief ZXFL boot protocol v3 — the contract between the bootloader and kernel.
///
///        The bootloader fills a zxfl_boot_protocol_t and passes a pointer
///        to it in R2 when jumping to the kernel entry point.  All pointer
///        fields are physical addresses (DAT is off at kernel entry).
///
///        KERNEL-BINDING MECHANISM
///        ========================
///        The loader computes a 64-bit binding_token at runtime:
///
///            binding_token = ZXFL_SEED ^ stfle_fac[0] ^ (uint64_t)ipl_schid
///
///        ZXFL_SEED is a 64-bit compile-time constant defined ONLY in the
///        loader's private header (arch/s390x/init/zxfl/zxvl_private.h).
///        It is never stored in this struct.  The kernel must independently
///        recompute the token using the same formula and the same seed to
///        verify authenticity.  An outsider's kernel has no seed and cannot
///        produce a valid token.

#ifndef ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H
#define ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H

#include <zxfoundation/types.h>
#include <arch/s390x/init/zxfl/stfle.h>

/// @brief Magic signature: ASCII "ZXFL" = 0x5A58464C
#define ZXFL_MAGIC              0x5A58464CU

/// @brief Protocol version
#define ZXFL_VERSION_3          0x00000003U

/// @brief Loader identity: major.minor
#define ZXFL_LOADER_MAJOR       0x0001U
#define ZXFL_LOADER_MINOR       0x0000U

/// @brief Protocol flags (zxfl_boot_protocol_t::flags)
#define ZXFL_FLAG_SMP           (1U << 0)   ///< cpu_map is valid
#define ZXFL_FLAG_MEM_MAP       (1U << 1)   ///< mem_map is valid
#define ZXFL_FLAG_CMDLINE       (1U << 2)   ///< cmdline_addr is valid
#define ZXFL_FLAG_LOWCORE       (1U << 3)   ///< lowcore_phys is valid
#define ZXFL_FLAG_STFLE         (1U << 4)   ///< stfle_fac[] is valid

/// @brief Maximum memory regions in the static map.
#define ZXFL_MEM_MAP_MAX        16U

/// @brief Maximum CPUs in the static cpu_map.
#define ZXFL_CPU_MAP_MAX        64U

/// @brief Memory region types.
#define ZXFL_MEM_USABLE         0x01U   ///< Conventional RAM
#define ZXFL_MEM_RESERVED       0x02U   ///< Firmware/loader reserved
#define ZXFL_MEM_LOADER         0x03U   ///< Occupied by the loader itself
#define ZXFL_MEM_KERNEL         0x04U   ///< Occupied by the loaded kernel

/// @brief CPU state values in zxfl_cpu_info_t::state.
#define ZXFL_CPU_ONLINE         0x01U   ///< CPU is running (BSP)
#define ZXFL_CPU_STOPPED        0x02U   ///< CPU is stopped (AP, ready for SIGP)
#define ZXFL_CPU_UNKNOWN        0x00U   ///< State could not be determined

// ---------------------------------------------------------------------------
// Auxiliary structures
// ---------------------------------------------------------------------------

/// @brief One entry in the physical memory map.
typedef struct {
    uint64_t base;      ///< Physical base address
    uint64_t length;    ///< Length in bytes
    uint32_t type;      ///< ZXFL_MEM_* constant
    uint32_t _pad;
} zxfl_mem_region_t;

/// @brief One entry in the CPU map.
typedef struct {
    uint16_t cpu_addr;  ///< z/Arch CPU address (used with SIGP)
    uint8_t  type;      ///< 0=IFL, 1=CP, 0xFF=unknown
    uint8_t  state;     ///< ZXFL_CPU_* constant
    uint32_t _pad;
} zxfl_cpu_info_t;

// ---------------------------------------------------------------------------
// Boot protocol
// ---------------------------------------------------------------------------

/// @brief Passed by pointer in R2 to the kernel entry point.
///        All pointer fields are valid physical addresses (DAT off).
///        The struct is 8-byte aligned; all uint64_t fields are at natural
///        offsets — no implicit compiler padding is needed or relied upon.
typedef struct {
    // ---- HEADER (16 bytes) ----
    uint32_t magic;             ///< ZXFL_MAGIC
    uint32_t version;           ///< ZXFL_VERSION_3
    uint32_t flags;             ///< ZXFL_FLAG_* bitmask
    uint32_t _pad0;

    // ---- BINDING TOKEN (8 bytes) ----
    /// Computed as: ZXFL_SEED ^ stfle_fac[0] ^ (uint64_t)ipl_schid.
    /// The kernel must recompute and compare to authenticate the loader.
    uint64_t binding_token;

    // ---- LOADER IDENTITY (16 bytes) ----
    uint16_t loader_major;      ///< ZXFL_LOADER_MAJOR
    uint16_t loader_minor;      ///< ZXFL_LOADER_MINOR
    uint32_t loader_timestamp;  ///< Compile-time timestamp (ZXFL_BUILD_TS macro)
    uint64_t loader_build_id;   ///< Reserved for future use; set to 0

    // ---- DEVICE (16 bytes) ----
    uint32_t ipl_schid;         ///< Subchannel ID of the IPL device
    uint16_t ipl_dev_type;      ///< Device type (e.g. 0x3390)
    uint16_t ipl_dev_model;     ///< Device model (e.g. 0x000C for 3390-12)
    uint32_t _pad1;
    uint32_t _pad2;

    // ---- KERNEL IMAGE (24 bytes) ----
    uint64_t kernel_phys_start; ///< Physical base of the loaded kernel image
    uint64_t kernel_phys_end;   ///< Physical end (exclusive) of the kernel image
    uint64_t kernel_entry;      ///< Actual entry point used (e_entry or relocated)

    // ---- MEMORY MAP (24 bytes) ----
    uint64_t mem_total_bytes;   ///< Total detected RAM (bytes)
    uint64_t mem_map_addr;      ///< Physical address of zxfl_mem_region_t[]
    uint32_t mem_map_count;     ///< Number of valid entries in mem_map
    uint32_t _pad3;

    // ---- FACILITIES (264 bytes) ----
    uint64_t stfle_fac[STFLE_MAX_DWORDS]; ///< Full STFLE output (32 dwords)
    uint32_t stfle_count;       ///< Actual dwords returned by STFLE
    uint32_t _pad4;

    // ---- LOWCORE (8 bytes) ----
    uint64_t lowcore_phys;      ///< Physical address of BSP lowcore (always 0x0)

    // ---- COMMAND LINE (16 bytes) ----
    uint64_t cmdline_addr;      ///< Physical address of NUL-terminated ASCII cmdline
    uint32_t cmdline_len;       ///< Length in bytes (excluding NUL)
    uint32_t _pad6;

    // ---- LOADER-PROVIDED KERNEL STACK (8 bytes) ----
    /// The loader allocates a 16KB stack for the kernel's initial thread.
    /// head64.S uses this instead of its own BSS stack, so the stack
    /// layout is controlled by the loader. An opaque frame is written
    /// below this address (see ZXFL_STACK_FRAME_* in zxvl_private.h).
    uint64_t kernel_stack_top;  ///< Physical address of initial kernel stack top

    // ---- CONTROL REGISTER SNAPSHOT (16 bytes) ----
    /// Taken after the loader configures CRs, immediately before kernel jump.
    uint64_t cr0_snapshot;
    uint64_t cr14_snapshot;
} zxfl_boot_protocol_t;

#endif /* ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H */

/// @brief Setup minimal page tables and enable DAT in loader.
void zxfl_mmu_setup(void);

/// @brief Jump to kernel with DAT enabled.
[[noreturn]] void zxfl_mmu_jump_kernel(uint64_t entry, uint64_t boot_proto);

/// @brief Setup page tables, enable DAT, and jump to kernel (combined).
[[noreturn]] void zxfl_mmu_setup_and_jump(uint64_t entry, uint64_t boot_proto);
