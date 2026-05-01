// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/zxfl.h
//
/// @brief ZXFL boot protocol — the contract between the bootloader and kernel.
///        The bootloader fills a zxfl_boot_protocol_t and passes a pointer
///        to it in R2 when jumping to __zx_start_kernel64.

#ifndef ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H
#define ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H

#include <zxfoundation/types.h>

/// @brief Magic signature: ASCII "ZXFL" = 0x5A58464C
#define ZXFL_MAGIC          0x5A58464CU

/// @brief Protocol version
#define ZXFL_VERSION_1      0x00000001U

// ---------------------------------------------------------------------------
// Memory region type codes
// ---------------------------------------------------------------------------
#define ZXFL_MEM_AVAILABLE  1U  ///< Usable RAM
#define ZXFL_MEM_RESERVED   2U  ///< Reserved (firmware, MMIO, etc.)
#define ZXFL_MEM_KERNEL     3U  ///< Kernel image (may be reclaimed later)
#define ZXFL_MEM_RAMDISK    4U  ///< Initial ramdisk
#define ZXFL_MEM_LOADER     5U  ///< Bootloader footprint (reclaimable)

/// @brief Maximum number of memory map entries in the static array.
#define ZXFL_MMAP_MAX_ENTRIES   16U

// ---------------------------------------------------------------------------
// Memory map entry
// ---------------------------------------------------------------------------

/// @brief One contiguous physical memory region.
typedef struct {
    uint64_t start;     ///< Physical start address
    uint64_t size;      ///< Size in bytes
    uint32_t type;      ///< ZXFL_MEM_* type code
    uint32_t _pad;      ///< Alignment padding
} zxfl_mmap_entry_t;

// ---------------------------------------------------------------------------
// Boot protocol
// ---------------------------------------------------------------------------

/// @brief Passed by pointer in R2 to __zx_start_kernel64.
///        All pointer fields are valid physical addresses (no MMU active).
typedef struct {
    uint32_t magic;             ///< ZXFL_MAGIC
    uint32_t version;           ///< ZXFL_VERSION_1

    // I/O & device identification
    uint32_t ipl_schid;         ///< Subchannel ID of the IPL device
    uint32_t ipl_dev_type;      ///< Device type (e.g. 0x3390)

    // Kernel image location
    uint64_t kernel_start;      ///< Physical base of the loaded kernel
    uint64_t kernel_size;       ///< Total byte span of the kernel image

    // Initial ramdisk (0 if not present)
    uint64_t ramdisk_start;
    uint64_t ramdisk_size;

    // Bootloader footprint (for reclamation by the kernel)
    uint64_t loader_start;
    uint64_t loader_size;

    // Hardware facilities (STFLE output, 256 bits)
    uint64_t stfle_fac[4];

    // Physical memory map
    uint32_t mmap_count;        ///< Number of valid entries in *mmap
    uint32_t _pad;
    zxfl_mmap_entry_t *mmap;    ///< Pointer to the memory map array

    // Kernel command line
    char *cmdline;              ///< NUL-terminated ASCII command line
} zxfl_boot_protocol_t;

#endif /* ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H */
