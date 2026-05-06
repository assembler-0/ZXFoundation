// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/vmalloc.h
//
/// @brief Kernel virtual memory allocator — vmalloc, vfree, ioremap.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/memory/vmm.h>

/// @brief Allocate virtually-contiguous, physically-discontiguous memory.
///        Backed by PMM pages mapped into the vmalloc region.
///        Use for large (> 128 KB) or variable-size kernel allocations.
/// @param size  Byte count (rounded up to PAGE_SIZE).
/// @return Kernel virtual address, or nullptr on OOM.
void *vmalloc(size_t size);

/// @brief Free memory returned by vmalloc().
/// @param ptr  Exact pointer returned by vmalloc(), or nullptr (no-op).
void vfree(void *ptr);

/// @brief Allocate and zero-fill virtually-contiguous memory.
/// @param size  Byte count.
/// @return Kernel virtual address, or nullptr on OOM.
void *vzalloc(size_t size);

/// @brief Map a physical MMIO region into kernel virtual space.
///        Does NOT allocate PMM pages; the physical range must be
///        reserved (e.g., PCI BAR, crypto adapter registers).
/// @param phys_addr  Physical base address (page-aligned).
/// @param size       Byte length (page-aligned).
/// @return Kernel virtual address, or nullptr on failure.
void *ioremap(uint64_t phys_addr, size_t size);

/// @brief Unmap an ioremap() region.
/// @param virt  Exact pointer returned by ioremap(), or nullptr (no-op).
void iounmap(void *virt);

/// @brief Returns true if 'ptr' lives in the vmalloc virtual region.
///        Used by kvfree() to dispatch without a separate tag.
static inline bool is_vmalloc_addr(const void *ptr) {
    uint64_t va = (uint64_t)(uintptr_t)ptr;
    return va >= VMALLOC_START && va < VMALLOC_END;
}