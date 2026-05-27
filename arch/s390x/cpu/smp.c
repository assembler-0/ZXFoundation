// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/smp.c
//
/// @brief SMP bringup

#include <arch/s390x/cpu/smp.h>
#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/cpu/processor.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <zxfoundation/time/ktime.h>
#include <arch/s390x/cpu/ipi.h>
#include <arch/s390x/cpu/irq.h>

extern void ap_entry(void);
extern void ap_dat_on(void);

static volatile uint32_t ap_online_count;

/// @brief Record of a successfully prepared AP, consumed by smp_start_aps().
typedef struct {
    uint16_t cpu_id;   ///< Logical CPU ID (index into percpu_areas[]).
    uint16_t cpu_addr; ///< Hardware CPU address (for SIGP).
    uint32_t lc_phys;  ///< Physical address of the allocated lowcore.
} ap_record_t;

static ap_record_t prepared_aps[MAX_CPUS];
static uint32_t    prepared_ap_count;

[[noreturn]] void ap_startup(void) {
    arch_ipi_init();
    __atomic_add_fetch(&ap_online_count, 1u, __ATOMIC_SEQ_CST);
    time_init_ap();
    arch_local_irq_enable();
    while (true)
        arch_cpu_relax();
}

void smp_prepare_aps(const zxfl_boot_protocol_t *boot) {
    arch_ipi_init(); // BSP IPI setup; idempotent.

    prepared_ap_count = 0;

    if (!(boot->flags & ZXFL_FLAG_SMP) || boot->cpu_count <= 1)
        return;

    const uint64_t ap_entry_phys =
        hhdm_virt_to_phys((uint64_t)(uintptr_t)ap_entry);

    const uint64_t kernel_asce = zx_lowcore()->kernel_asce;
    uint64_t cr0;
    arch_ctl_store(cr0, 0, 0);

    for (uint32_t i = 0; i < boot->cpu_count; i++) {
        const zxfl_cpu_info_t *ci = &boot->cpu_map[i];
        if (ci->cpu_addr == boot->bsp_cpu_addr) continue;
        if (ci->state != ZXFL_CPU_STOPPED)      continue;

        // Allocate AP lowcore NUMA-local to the AP's book/node.
        uint8_t node = ci->numa_node;
        if (node >= NUMA_MAX_NODES) node = 0;

        uint64_t lc_phys = percpu_init_ap((uint16_t)i, ci->cpu_addr, node);
        if (!lc_phys) {
            printk(ZX_ERROR "smp: OOM allocating lowcore for cpu %u\n", i);
            continue;
        }

        // Allocate stacks NUMA-local to the AP so register saves hit local memory.
        zx_page_t *stack_page = pmm_alloc_page_node(node, ZX_GFP_ZERO);
        zx_page_t *async_page = pmm_alloc_page_node(node, ZX_GFP_ZERO);
        zx_page_t *mcck_page  = pmm_alloc_page_node(node, ZX_GFP_ZERO);
        if (!stack_page || !async_page || !mcck_page) {
            printk(ZX_ERROR "smp: OOM allocating stacks for cpu %u\n", i);
            if (stack_page) pmm_free_page(stack_page);
            if (async_page) pmm_free_page(async_page);
            if (mcck_page)  pmm_free_page(mcck_page);
            percpu_areas[i] = nullptr;
            continue;
        }

        zx_lowcore_t *lc = (zx_lowcore_t *)hhdm_phys_to_virt(lc_phys);

        lc->kernel_stack  = hhdm_phys_to_virt(pmm_page_to_phys(stack_page) + PAGE_SIZE);
        lc->restart_stack = pmm_page_to_phys(stack_page) + PAGE_SIZE;
        lc->async_stack   = hhdm_phys_to_virt(pmm_page_to_phys(async_page) + PAGE_SIZE);
        lc->mcck_stack    = hhdm_phys_to_virt(pmm_page_to_phys(mcck_page)  + PAGE_SIZE);

        lc->percpu.ap_stack_top = lc->restart_stack;

        lc_set_kernel_asce(lc, kernel_asce);
        lc_set_restart_psw(lc, ap_entry_phys);
        lc_install_handler_psws(lc);

        lc->ap_cr0  = cr0;
        lc->ap_cr13 = kernel_asce;

        lc->return_psw.mask = PSW_MASK_KERNEL_DAT;
        lc->return_psw.addr = (uint64_t)(uintptr_t)ap_dat_on;

        pmm_pcplist_init((uint16_t)i);

        // Record for phase 2.
        prepared_aps[prepared_ap_count].cpu_id   = (uint16_t)i;
        prepared_aps[prepared_ap_count].cpu_addr  = ci->cpu_addr;
        prepared_aps[prepared_ap_count].lc_phys   = (uint32_t)lc_phys;
        prepared_ap_count++;

        printk(ZX_DEBUG "smp: cpu %u (addr=0x%04x) prepared (lc=0x%llx entry=0x%llx node=%u)\n",
               i, ci->cpu_addr,
               (unsigned long long)lc_phys,
               (unsigned long long)ap_entry_phys,
               node);
    }

    printk(ZX_DEBUG "smp: %u APs prepared\n", prepared_ap_count);
}

void smp_start_aps(void) {
    if (prepared_ap_count == 0)
        return;

    const uint64_t kernel_asce = zx_lowcore()->kernel_asce;
    uint64_t cr0;
    arch_ctl_store(cr0, 0, 0);

    for (uint32_t i = 0; i < prepared_ap_count; i++) {
        uint16_t addr    = prepared_aps[i].cpu_addr;
        uint32_t lc_phys = prepared_aps[i].lc_phys;
        zx_lowcore_t *lc = (zx_lowcore_t *)hhdm_phys_to_virt(lc_phys);

        lc_set_kernel_asce(lc, kernel_asce);
        lc->ap_cr13 = kernel_asce;
        lc->ap_cr0  = cr0;

        sigp_busy(addr, SIGP_SET_PREFIX, lc_phys, nullptr);
        sigp_busy(addr, SIGP_RESTART,    0,        nullptr);

        printk(ZX_DEBUG "smp: cpu %u (addr=0x%04x) started\n",
               prepared_aps[i].cpu_id, addr);
    }

    // Spin until all APs have incremented ap_online_count from ap_startup().
    while (__atomic_load_n(&ap_online_count, __ATOMIC_SEQ_CST) < prepared_ap_count)
        arch_cpu_relax();

    printk(ZX_INFO "smp: all %u APs online\n", prepared_ap_count);
}

/// @brief Issue SIGP STOP to all CPUs except the caller.
void smp_teardown(void) {
    const uint16_t my_addr = arch_cpu_addr();

    for (unsigned int i = 0; i < MAX_CPUS; i++) {
        const zx_lowcore_t *cpu = percpu_areas[i];
        if (!cpu)
            continue;
        if (cpu->percpu.cpu_addr == my_addr)
            continue;
        sigp_busy(cpu->percpu.cpu_addr, SIGP_STOP, 0, nullptr);
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