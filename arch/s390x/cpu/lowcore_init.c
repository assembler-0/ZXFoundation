// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/lowcore_init.c

#include <arch/s390x/cpu/lowcore.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>

/// @brief Initialize BSP-specific stacks that require the PMM to be ready.
///        Allocates async_stack and mcck_stack for the BSP.
void zx_lowcore_init_late_bsp(void) {
    zx_lowcore_t *lc = zx_lowcore();

    zx_page_t *async_page = pmm_alloc_page(ZX_GFP_ZERO);
    zx_page_t *mcck_page  = pmm_alloc_page(ZX_GFP_ZERO);

    if (!async_page || !mcck_page)
        zx_system_check(ZX_SYSCHK_MEM_OOM, "lowcore: OOM allocating BSP stacks");

    lc->async_stack = hhdm_phys_to_virt(pmm_page_to_phys(async_page) + PAGE_SIZE);
    lc->mcck_stack  = hhdm_phys_to_virt(pmm_page_to_phys(mcck_page)  + PAGE_SIZE);

    printk(ZX_DEBUG "lowcore: BSP stacks initialized (async=0x%llx mcck=0x%llx)\n",
           (unsigned long long)lc->async_stack,
           (unsigned long long)lc->mcck_stack);
}
