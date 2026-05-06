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

/// Defined in head64.S — the AP restart entry point (virtual address).
extern void ap_entry(void);
/// Defined in head64.S — the label after lpswe where DAT is on (virtual address).
extern void ap_dat_on(void);

/// Count of APs that have reached ap_startup (DAT on, stack valid).
/// BSP polls this after issuing all SIGP Restarts.
static volatile uint32_t ap_online_count;

/// @brief C-level AP startup, called from ap_entry after DAT is on and
///        the virtual stack is set up.
///
///        At this point:
///          - CR1 is loaded with the kernel ASCE (DAT on, HHDM accessible)
///          - %r15 is the HHDM virtual stack pointer
///          - All interrupts are still disabled
[[noreturn]] void ap_startup(void) {
    // Signal the BSP that this AP is alive and running with DAT on.
    // __atomic_add_fetch with __ATOMIC_SEQ_CST ensures the BSP sees the
    // increment only after all prior stores (stack setup, CR1 load) are
    // globally visible.
    __atomic_add_fetch(&ap_online_count, 1u, __ATOMIC_SEQ_CST);

    // TODO: enable external interrupts, set up per-CPU interrupt handlers.
    while (true)
        arch_cpu_relax();
}

void smp_init(const zxfl_boot_protocol_t *boot) {
    if (!(boot->flags & ZXFL_FLAG_SMP) || boot->cpu_count <= 1)
        return;

    // ap_entry is linked at a virtual address in the HHDM.  The AP starts
    // with DAT OFF, so the restart PSW must carry the *physical* address.
    const uint64_t ap_entry_phys =
        hhdm_virt_to_phys((uint64_t)(uintptr_t)ap_entry);

    // The kernel ASCE (CR1 value) was captured by mmu_init() and stored in
    // the BSP's lowcore.  Every AP needs it to enable DAT in ap_entry.
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

        // Allocate a 4 KB initial stack.  Store the physical top in the
        // per-CPU area so ap_entry can load it before DAT is on.
        // After DAT is enabled, ap_startup uses the HHDM virtual address.
        zx_page_t *stack_page = pmm_alloc_page(ZX_GFP_ZERO);
        if (!stack_page) {
            printk("smp: OOM allocating stack for cpu %u\n", i);
            continue;
        }
        const uint64_t stack_phys_top = pmm_page_to_phys(stack_page) + PAGE_SIZE;

        zx_lowcore_t *lc = zx_lowcore_of(lc_phys);

        // kernel_stack holds the HHDM virtual address; ap_startup uses it
        // after DAT is on.
        lc->kernel_stack = hhdm_phys_to_virt(stack_phys_top);

        // restart_stack holds the *physical* top so ap_entry can set %r15
        // before DAT is enabled.  Using a lowcore field (fixed offset 0x0360)
        // avoids hardcoding offsetof(percpu_t, ap_stack_top) in assembly.
        lc->restart_stack = stack_phys_top;

        // The ASCE must be in the lowcore before SIGP Restart so ap_entry
        // can load it with a prefix-relative load (no DAT needed).
        lc_set_kernel_asce(lc, kernel_asce);

        // Restart PSW: DAT OFF, 64-bit, physical address of ap_entry.
        // DAT is off because CR1 is not yet loaded on the AP.
        lc_set_restart_psw(lc, ap_entry_phys);

        // DAT-on transition PSW at lowcore.return_psw (offset 0x0290).
        // ap_entry issues lpswe 0x290(0) to jump here with DAT enabled.
        lc->return_psw.mask = PSW_MASK_KERNEL_DAT;
        lc->return_psw.addr = (uint64_t)(uintptr_t)ap_dat_on;

        pmm_pcplist_init((uint16_t)i);

        // SIGP Set Prefix: redirect the AP's lowcore to lc_phys.
        // SIGP Restart:    load restart_psw and begin execution.
        sigp_busy(ci->cpu_addr, SIGP_SET_PREFIX, (uint32_t)lc_phys, nullptr);
        sigp_busy(ci->cpu_addr, SIGP_RESTART,    0,                  nullptr);

        printk("smp: cpu %u (addr=0x%04x) started (lc=0x%llx entry=0x%llx)\n",
               i, ci->cpu_addr,
               (unsigned long long)lc_phys,
               (unsigned long long)ap_entry_phys);
        ap_count++;
    }

    // Spin until all launched APs have signalled back from ap_startup.
    // This ensures the BSP does not proceed past smp_init() while APs are
    // still in the pre-DAT bringup sequence.
    while (__atomic_load_n(&ap_online_count, __ATOMIC_SEQ_CST) < ap_count)
        arch_cpu_relax();

    printk("smp: all %u APs online\n", ap_count);
}
