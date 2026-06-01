// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/hhdm.h - HHDM constants and helpers

#pragma once

#include <zxfoundation/types.h>

// Higher-Half Direct Map (HHDM) offset.
#define CONFIG_KERNEL_VIRT_OFFSET           0xFFFF800000000000ULL

/// @brief Convert physical address to Higher-Half Direct Map (HHDM) virtual address.
#define hhdm_phys_to_virt_inplace(phys) ((phys) + CONFIG_KERNEL_VIRT_OFFSET)

/// @brief Convert Higher-Half Direct Map (HHDM) virtual address to physical address.
#define hhdm_virt_to_phys_inplace(virt) ((virt) - CONFIG_KERNEL_VIRT_OFFSET)

/// @brief Convert physical address to Higher-Half Direct Map (HHDM) virtual address.
static inline uint64_t hhdm_phys_to_virt(uint64_t phys) {
    return phys + CONFIG_KERNEL_VIRT_OFFSET;
}

/// @brief Convert Higher-Half Direct Map (HHDM) virtual address to physical address.
static inline uint64_t hhdm_virt_to_phys(uint64_t virt) {
    return virt - CONFIG_KERNEL_VIRT_OFFSET;
}
