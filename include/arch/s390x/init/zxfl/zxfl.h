// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/zxfl.h
//
/// @brief ZXFL boot protocol — the contract between the bootloader and kernel.
///        The bootloader fills a zxfl_boot_protocol_t and passes a pointer
///        to it in R2 when jumping to the kernel entry point.

#ifndef ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H
#define ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H

#include <zxfoundation/types.h>

/// @brief Magic signature: ASCII "ZXFL" = 0x5A58464C
#define ZXFL_MAGIC              0x5A58464CU

/// @brief Protocol version
#define ZXFL_VERSION_2          0x00000002U


/// @brief Passed by pointer in R2 to the kernel entry point.
///        All pointer fields are valid physical addresses (DAT off).
typedef struct {
    uint32_t magic;             ///< ZXFL_MAGIC
    uint32_t version;           ///< ZXFL_VERSION_2

    // I/O & device identification
    uint32_t ipl_schid;         ///< Subchannel ID of the IPL device
    uint32_t ipl_dev_type;      ///< Device type (e.g. 0x3390)

    // Kernel image location (physical)
    uint64_t kernel_phys_start; ///< Physical base of the loaded kernel image
    uint64_t kernel_size;       ///< Total byte span of the kernel image

    // Hardware facilities (STFLE output, up to 256 bits = 4 doublewords)
    uint64_t stfle_fac[4];
    uint32_t fac_count;

    // Kernel command line (ASCII, NUL-terminated)
    char *cmdline;
} zxfl_boot_protocol_t;

#endif /* ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H */
