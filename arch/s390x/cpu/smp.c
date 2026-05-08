// SPDX-License-Identifier: Apache-2.0
// arch/s390x/smp.c

#include <arch/s390x/cpu/smp.h>
#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/mmu/mmu.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>

extern void ap_entry(void);
extern void ap_dat_on(void);

static volatile uint32_t ap_online_count;

[[noreturn]] void ap_startup(void) {
    __atomic_add_fetch(&ap_online_count, 1u, __ATOMIC_SEQ_CST);
    while (true)
        arch_cpu_relax();
}

void smp_init(const zxfl_boot_protocol_t *boot) {
    if (!(boot->flags & ZXFL_FLAG_SMP) || boot->cpu_count <= 1)
        return;

    const uint64_t ap_entry_phys =
        hhdm_virt_to_phys((uint64_t)(uintptr_t)ap_entry);

    const uint64_t kernel_asce = zx_lowcore()->kernel_asce;

    uint32_t ap_count = 0;

    for (uint32_t i = 0; i < boot->cpu_count; i++) {
        const zxfl_cpu_info_t *ci = &boot->cpu_map[i];
        if (ci->cpu_addr == boot->bsp_cpu_addr) continue;
        if (ci->state != ZXFL_CPU_STOPPED)      continue;

        uint64_t lc_phys = percpu_init_ap((uint16_t)i, ci->cpu_addr);
        if (!lc_phys) {
            printk("smp: OOM allocating lowcore for cpu %u\n", i);
            continue;
        }

        zx_page_t *stack_page = pmm_alloc_page(ZX_GFP_ZERO);
        zx_page_t *async_page = pmm_alloc_page(ZX_GFP_ZERO);
        zx_page_t *mcck_page  = pmm_alloc_page(ZX_GFP_ZERO);
        if (!stack_page || !async_page || !mcck_page) {
            printk("smp: OOM allocating stacks for cpu %u\n", i);
            if (stack_page) pmm_free_page(stack_page);
            if (async_page) pmm_free_page(async_page);
            if (mcck_page)  pmm_free_page(mcck_page);
            continue;
        }

        zx_lowcore_t *lc = zx_lowcore_of(lc_phys);

        lc->kernel_stack  = hhdm_phys_to_virt(pmm_page_to_phys(stack_page) + PAGE_SIZE);
        lc->restart_stack = pmm_page_to_phys(stack_page) + PAGE_SIZE;
        lc->async_stack   = hhdm_phys_to_virt(pmm_page_to_phys(async_page) + PAGE_SIZE);
        lc->mcck_stack    = hhdm_phys_to_virt(pmm_page_to_phys(mcck_page)  + PAGE_SIZE);

        lc_set_kernel_asce(lc, kernel_asce);
        lc_set_restart_psw(lc, ap_entry_phys);
        lc_install_handler_psws(lc);

        lc->return_psw.mask = PSW_MASK_KERNEL_DAT;
        lc->return_psw.addr = (uint64_t)(uintptr_t)ap_dat_on;

        pmm_pcplist_init((uint16_t)i);

        sigp_busy(ci->cpu_addr, SIGP_SET_PREFIX, (uint32_t)lc_phys, nullptr);
        sigp_busy(ci->cpu_addr, SIGP_RESTART,    0,                  nullptr);

        printk("smp: cpu %u (addr=0x%04x) started (lc=0x%llx entry=0x%llx)\n",
               i, ci->cpu_addr,
               (unsigned long long)lc_phys,
               (unsigned long long)ap_entry_phys);
        ap_count++;
    }

    while (__atomic_load_n(&ap_online_count, __ATOMIC_SEQ_CST) < ap_count)
        arch_cpu_relax();

    printk("smp: all %u APs online\n", ap_count);
}

/// @brief Issue SIGP STOP to all CPUs except the caller.
///        Uses sigp_busy() so CC=2 (busy) is retried.
///        CC=3 (not operational) is silently ignored.
void smp_teardown(void) {
    const uint16_t my_addr = arch_cpu_addr();

    for (unsigned int i = 0; i < MAX_CPUS; i++) {
        const percpu_t *cpu = percpu_areas[i];
        if (!cpu)
            continue;
        if (cpu->cpu_addr == my_addr)
            continue;
        sigp_busy(cpu->cpu_addr, SIGP_STOP, 0, nullptr);
    }
}

/// @brief Lock-free SIGP STOP over the boot protocol CPU map.
///        Does not touch percpu_areas[]; safe from the halt path.
void smp_stop_all_raw(const zxfl_cpu_info_t *cpu_map, uint32_t cpu_count) {
    const uint16_t my_addr = arch_cpu_addr();
    for (uint32_t i = 0; i < cpu_count; i++) {
        const uint16_t addr = cpu_map[i].cpu_addr;
        if (addr == my_addr)
            continue;
        int cc;
        do { cc = sigp(addr, SIGP_STOP, 0, nullptr); }
        while (cc == SIGP_CC_BUSY);
    }
}