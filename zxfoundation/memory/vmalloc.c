// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/vmalloc.c
//
/// @brief vmalloc / ioremap — large kernel virtual allocations.

#include <zxfoundation/memory/vmalloc.h>
#include <zxfoundation/memory/vmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/zconfig.h>

/// Guard canary embedded before the user pointer to detect underflows.
#define VMALLOC_MAGIC  0x564D414C4C4F4355ULL   /* "VMALLOCU" */

typedef struct {
    uint64_t magic;       ///< Must equal VMALLOC_MAGIC.
    uint64_t total_size;  ///< Total bytes passed to vmm_alloc(), including header.
} vmalloc_hdr_t;

_Static_assert(sizeof(vmalloc_hdr_t) == 16, "vmalloc_hdr_t must be 16 bytes");

void *vmalloc(size_t size) {
    if (!size) return nullptr;

    uint64_t total = (uint64_t)sizeof(vmalloc_hdr_t) + (uint64_t)size;
    total = (total + PAGE_SIZE - 1) & PAGE_MASK;

    uint64_t base = vmm_alloc(total, VM_READ | VM_WRITE | VM_KERNEL, ZX_GFP_NORMAL);
    if (!base) return nullptr;

    vmalloc_hdr_t *hdr = (vmalloc_hdr_t *)(uintptr_t)base;
    hdr->magic      = VMALLOC_MAGIC;
    hdr->total_size = total;

    return (void *)(uintptr_t)(base + sizeof(vmalloc_hdr_t));
}

void vfree(void *ptr) {
    if (!ptr) return;

    vmalloc_hdr_t *hdr = (vmalloc_hdr_t *)((uintptr_t)ptr - sizeof(vmalloc_hdr_t));

    if (hdr->magic != VMALLOC_MAGIC)
        panic("vfree: corrupted vmalloc header at %p (magic=%016llx)",
              ptr, (unsigned long long)hdr->magic);

    uint64_t base = (uint64_t)(uintptr_t)hdr;
    hdr->magic    = 0; // Poison to catch use-after-free.
    vmm_free(base);
}

void *vzalloc(size_t size) {
    void *p = vmalloc(size);
    if (!p) return nullptr;
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < size; i++) b[i] = 0;
    return p;
}

/// @brief ioremap — map a physical MMIO range into the vmalloc region.
///        z/Architecture devices that expose MMIO (PCI passthrough, AP bus
///        crypto adapters) require this.  Channel-program I/O devices do NOT
///        use MMIO and should not call this function.
///        The mapping uses VM_IOREMAP so vmm_remove_vma() skips PMM freeing.
void *ioremap(uint64_t phys_addr, size_t size) {
    if (!size) return nullptr;

    // Both base and size must be page-aligned for the MMU.
    uint64_t aligned_phys = phys_addr & PAGE_MASK;
    uint64_t offset       = phys_addr - aligned_phys;
    uint64_t aligned_size = (size + offset + PAGE_SIZE - 1) & PAGE_MASK;

    uint64_t va = vmm_map_phys(aligned_phys, aligned_size,
                               VM_READ | VM_WRITE | VM_KERNEL);
    if (!va) return nullptr;

    return (void *)(uintptr_t)(va + offset);
}

void iounmap(void *virt) {
    if (!virt) return;
    // Align back to the page boundary that vmm_map_phys mapped.
    uint64_t va = (uint64_t)(uintptr_t)virt & PAGE_MASK;
    vmm_free(va);
}
