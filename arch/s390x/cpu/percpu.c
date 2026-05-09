// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/percpu.c

#include <zxfoundation/percpu.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/zconfig.h>
#include <zxfoundation/sys/syschk.h>
#include <lib/string.h>

percpu_t *percpu_areas[MAX_CPUS];

/// @brief BSP lowcore is always at physical 0x0.
///        The per-CPU block is at physical 0x0 + PERCPU_OFFSET.
void percpu_init_bsp(void) {
    percpu_t *p = (percpu_t *)hhdm_phys_to_virt(PERCPU_OFFSET);
    memset(p, 0, sizeof(*p));

    uint16_t cpu_addr;
    __asm__ volatile ("stap %0" : "=Q" (cpu_addr));

    p->prefix_base = 0;
    p->cpu_id      = 0;
    p->cpu_addr    = cpu_addr;
    p->lock_depth  = 0;

    percpu_areas[0] = p;
}

/// @brief Allocate and initialize a per-CPU area for an AP.
/// @param cpu_id   Logical CPU ID (1..MAX_CPUS-1).
/// @param cpu_addr z/Arch CPU address (from boot protocol).
/// @return Physical address of the new lowcore (8 KB, 4 KB-aligned), or 0 on failure.
uint64_t percpu_init_ap(uint16_t cpu_id, uint16_t cpu_addr) {
    if (cpu_id >= MAX_CPUS)
        zx_system_check(ZX_SYSCHK_CORE_ASSERT, "percpu: cpu_id %u exceeds MAX_CPUS", cpu_id);

    zx_page_t *page = pmm_alloc_pages(1, ZX_GFP_ZERO);
    if (!page)
        zx_system_check(ZX_SYSCHK_MEM_OOM, "percpu: OOM allocating lowcore for cpu %u", cpu_id);

    uint64_t phys = pmm_page_to_phys(page);
    percpu_t *p   = (percpu_t *)hhdm_phys_to_virt(phys + PERCPU_OFFSET);

    p->prefix_base = phys;
    p->cpu_id      = cpu_id;
    p->cpu_addr    = cpu_addr;
    p->lock_depth  = 0;

    percpu_areas[cpu_id] = p;
    return phys;
}
