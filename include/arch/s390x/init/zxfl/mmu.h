#pragma once

#include <zxfoundation/types.h>

/// @brief Setup page tables, enable DAT, and jump to kernel.
/// @param entry      Kernel entry point (HHDM virtual address from ELF).
/// @param boot_proto Physical address of zxfl_boot_protocol_t.
[[noreturn]] void zxfl_mmu_setup_and_jump(uint64_t entry, uint64_t boot_proto);