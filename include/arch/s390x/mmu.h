// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/mmu.h

#pragma once

#include <zxfoundation/types.h>

/// @brief Initialize the hardware DAT structures.
void arch_mmu_init(void);

/// @brief Identity map a range of physical memory.
void arch_mmu_map_identity(uint64_t phys_start, uint64_t size);

/// @brief Activate Dynamic Address Translation (DAT).
[[noreturn]] void arch_mmu_activate(void);
