// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/percpu.c

#include <zxfoundation/percpu.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/zconfig.h>
#include <zxfoundation/sys/panic.h>
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

/// @brief Allocate a fresh 4 KB page for an AP's lowcore.
///        The page is zeroed, the per-CPU block is initialised, and the
///        physical address is returned so the caller can issue SPX.
uint64_t percpu_init_ap(uint16_t cpu_id, uint16_t cpu_addr) {
    if (cpu_id >= MAX_CPUS)
        panic("percpu: cpu_id %u exceeds MAX_CPUS", cpu_id);

    // Allocate one 4 KB page for the AP's lowcore.
    zx_page_t *page = pmm_alloc_page(ZX_GFP_ZERO);
    if (!page)
        panic("percpu: OOM allocating lowcore for cpu %u", cpu_id);

    uint64_t phys = pmm_page_to_phys(page);
    percpu_t *p   = (percpu_t *)hhdm_phys_to_virt(phys + PERCPU_OFFSET);

    p->prefix_base = phys;
    p->cpu_id      = cpu_id;
    p->cpu_addr    = cpu_addr;
    p->lock_depth  = 0;

    percpu_areas[cpu_id] = p;
    return phys;
}
