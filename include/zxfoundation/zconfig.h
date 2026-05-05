#ifndef ZXFOUNDATION_ZCONFIG_H
#define ZXFOUNDATION_ZCONFIG_H

// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/zconfig.h

// ---------------------------------------------------------------------------
// Release
// ---------------------------------------------------------------------------
#define CONFIG_ZX_RELEASE           "26h1"

// ---------------------------------------------------------------------------
// Console driverX
// ---------------------------------------------------------------------------
#define CONFIG_SCLP_CONSOLE                 1
#define CONFIG_SCLP_SERVC_MAX_RETRIES       100
#define CONFIG_SCLP_SERVC_BUSY_DELAY        100000

// ---------------------------------------------------------------------------
// s390x init
// ---------------------------------------------------------------------------
#define CONFIG_BOOT_STACK_SIZE              16384
#define CONFIG_S390X_SAVE_AREA              160

// ---------------------------------------------------------------------------
// s390x PSW constants
// (64-bit values — use ULL suffix so they are safe in both C and assembly)
// ---------------------------------------------------------------------------
#define CONFIG_PSW_ARCH_BITS                0x0000000180000000ULL
#define CONFIG_PSW_DISABLED_WAIT            0x0000800180000000ULL

// ---------------------------------------------------------------------------
// Trap / panic
// ---------------------------------------------------------------------------
#define CONFIG_PANIC_HALT_ADDR              0x0000000000DEAD00ULL

// ---------------------------------------------------------------------------
// Computed / derived
// ---------------------------------------------------------------------------
#define CONFIG_HAVE_CONSOLE                 1
#define CONFIG_PAGE_SIZE                    4096UL

/// @brief Higher-Half Direct Map (HHDM) offset.
///        0xFFFF800000000000ULL places the kernel at -128TB, in the
///        topmost Region-First entry (RFX=2047) of the 5-level paging
///        hierarchy.  This cleanly separates kernel (R1[2047]) from
///        user space (R1[0]) at the highest table level.
#define CONFIG_KERNEL_VIRT_OFFSET           0xFFFF800000000000ULL

#ifndef __ASSEMBLER__

#include <zxfoundation/types.h>

/// @brief Convert physical address to Higher-Half Direct Map (HHDM) virtual address.
static inline uint64_t hhdm_phys_to_virt(uint64_t phys) {
    return phys + CONFIG_KERNEL_VIRT_OFFSET;
}

/// @brief Convert Higher-Half Direct Map (HHDM) virtual address to physical address.
static inline uint64_t hhdm_virt_to_phys(uint64_t virt) {
    return virt - CONFIG_KERNEL_VIRT_OFFSET;
}
#endif

#endif /* ZXFOUNDATION_ZCONFIG_H */
