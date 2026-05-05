// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/mmu.h
#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/zconfig.h>

/// @brief Setup page tables, enable DAT, and jump to kernel.
/// @param entry      Kernel entry point (HHDM virtual address from ELF).
/// @param boot_proto Physical address of zxfl_boot_protocol_t.
[[noreturn]] void zxfl_mmu_setup_and_jump(uint64_t entry, uint64_t boot_proto);