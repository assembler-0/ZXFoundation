// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/vm_flags.h
//
/// @brief VM protection / mapping flags — shared between mmu.h and vmm.h.
///        Kept in a standalone header so arch/s390x/mmu.h can include it
///        without pulling in the full VMM.

#pragma once

#include <zxfoundation/types.h>

typedef uint32_t vm_prot_t;

#define VM_READ     (1U << 0)   ///< Region is readable.
#define VM_WRITE    (1U << 1)   ///< Region is writable.
#define VM_EXEC     (1U << 2)   ///< Region is executable.
#define VM_KERNEL   (1U << 3)   ///< Region belongs to kernel space.
#define VM_IOREMAP  (1U << 4)   ///< Maps MMIO (not backed by PMM frames).
#define VM_FIXED    (1U << 5)   ///< Must not be moved or merged.
#define VM_HUGEPAGE (1U << 6)   ///< Use 1 MB EDAT-1 large pages if available.
#define VM_SHARED   (1U << 7)   ///< Region is shared (future use).
