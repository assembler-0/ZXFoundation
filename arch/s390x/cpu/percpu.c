// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/percpu.c

#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/cpu/lowcore.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/zxconfig.h>
#include <zxfoundation/sys/syschk.h>
#include <lib/string.h>

zx_lowcore_t *percpu_areas[MAX_CPUS];

/// @brief BSP lowcore is always at physical 0x0.
///        The monolithic structure is 8 KB, but only the first page (4 KB)
///        is guaranteed to be cleared by the loader at physical 0x0.
void percpu_init_bsp(void) {
    zx_lowcore_t *lc = (zx_lowcore_t *)hhdm_phys_to_virt(0x0);

    // We do NOT memset the whole lc because it contains the boot protocol,
    // interrupt PSWs, and other fields already initialized.
    // We only initialize our software per-CPU block.
    memset(&lc->percpu, 0, sizeof(lc->percpu));

    const uint16_t cpu_addr = arch_cpu_addr();

    lc->percpu.prefix_base = 0;
    lc->percpu.cpu_id      = 0;
    lc->percpu.cpu_addr    = cpu_addr;
    lc->percpu.lock_depth  = 0;

    percpu_areas[0] = lc;
}

/// @brief Allocate and initialize a monolithic lowcore for an AP.
/// @param cpu_id   Logical CPU ID (1..MAX_CPUS-1).
/// @param cpu_addr z/Arch CPU address (from boot protocol).
/// @return Physical address of the new lowcore (8 KB, 8 KB-aligned), or 0 on failure.
uint64_t percpu_init_ap(uint16_t cpu_id, uint16_t cpu_addr, uint8_t node) {
    if (cpu_id >= MAX_CPUS)
        zx_system_check(ZX_SYSCHK_CORE_ASSERT, "percpu: cpu_id %u exceeds MAX_CPUS", cpu_id);

    zx_page_t *page = pmm_alloc_pages_node(node, 1, ZX_GFP_ZERO | ZX_GFP_DMA);
    if (!page)
        zx_system_check(ZX_SYSCHK_MEM_OOM, "percpu: OOM allocating lowcore for cpu %u", cpu_id);

    uint64_t phys = pmm_page_to_phys(page);
    zx_lowcore_t *lc = (zx_lowcore_t *)hhdm_phys_to_virt(phys);

    lc->percpu.prefix_base = phys;
    lc->percpu.cpu_id      = cpu_id;
    lc->percpu.cpu_addr    = cpu_addr;
    lc->percpu.lock_depth  = 0;

    percpu_areas[cpu_id] = lc;
    return phys;
}

