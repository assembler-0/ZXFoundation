// SPDX-License-Identifier: Apache-2.0
// zxfoundation/init/main.c — ZXFoundation kernel entry point

#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/vmm.h>
#include <zxfoundation/memory/slab.h>
#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/heap.h>
#include <arch/s390x/mmu.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/zxvl_private.h>
#include <arch/s390x/init/zxfl/lowcore.h>
#include <arch/s390x/cpu/processor.h>
#include <drivers/console/diag.h>
#include <lib/string.h>

/// @brief Called from head64.S to extract the loader-provided stack top.
///        Returning 0 causes head64.S to fall back to the BSS stack.
uint64_t zx_get_loader_stack_top(const zxfl_boot_protocol_t *boot) {
    if (!boot || boot->magic != ZXFL_MAGIC)
        return 0;
    return boot->kernel_stack_top;
}

/// @brief Validate the opaque stack frame written by the loader.
///        The frame sits at boot->kernel_stack_top (the loader set
///        kernel_stack_top to point at the frame, not above it).
static void validate_stack_frame(const zxfl_boot_protocol_t *boot) {
    if (!boot->kernel_stack_top)
        goto bad;

    const uint64_t *frame = (const uint64_t *)boot->kernel_stack_top;
    if (frame[0] != ZXVL_FRAME_MAGIC_A)
        goto bad;

    const uint64_t expected_b = ZXVL_FRAME_MAGIC_B ^ boot->binding_token;
    if (frame[1] != expected_b)
bad:
        panic("sys: stack frame corruption/not found — unauthorized loader");
}

[[noreturn]] void zxfoundation_global_initialize(zxfl_boot_protocol_t *boot) {
    // Install HHDM-virtual disabled-wait PSWs into the lowcore FIRST.
    // Any fault before this point (including inside diag_setup) would load
    // the loader's physical-address PSW and cause a program interrupt loop.
    zxfl_lowcore_setup();

    diag_setup();
    printk_initialize(diag_putc);

    if (!boot || boot->magic != ZXFL_MAGIC)
        panic("sys: protocol missing or corrupt");

    const uint64_t expected = ZXVL_COMPUTE_TOKEN(boot->stfle_fac[0], boot->ipl_schid);
    if (boot->binding_token != expected)
        panic("sys: binding token mismatch — unauthorized loader");

    validate_stack_frame(boot);

    printk("sys: ZXFoundation %s copyright (C) 2026 assembler-0 All rights reserved.\n",
           CONFIG_ULTRASPARK_RELEASE);

    if (boot->flags & ZXFL_FLAG_SYSINFO) {
        printk("sys: machine: %s %s model %s (s/n %s) plant %s\n",
               boot->sysinfo.manufacturer,
               boot->sysinfo.type,
               boot->sysinfo.model,
               boot->sysinfo.sequence,
               boot->sysinfo.plant);
        printk("sys: lpar: %s (id %u)\n",
               boot->sysinfo.lpar_name[0] ? boot->sysinfo.lpar_name : "<bare-metal>",
               boot->sysinfo.lpar_number);
        printk("sys: cpus: %u total, %u configured, %u standby (rating %u)\n",
               boot->sysinfo.cpus_total,
               boot->sysinfo.cpus_configured,
               boot->sysinfo.cpus_standby,
               boot->sysinfo.capability);
    }

    if (boot->flags & ZXFL_FLAG_SMP) {
        printk("sys: smp: %u processors detected\n", boot->cpu_count);
        for (uint32_t i = 0; i < boot->cpu_count; i++) {
            printk("     cpu[%u] addr=0x%04x state=%u bsp=%s\n",
                   i,
                   boot->cpu_map[i].cpu_addr,
                   boot->cpu_map[i].state,
                   boot->cpu_map[i].cpu_addr == boot->bsp_cpu_addr ? "yes" : "no");
        }
    }

    if (boot->flags & ZXFL_FLAG_TOD) {
        printk("sys: tod: 0x%016llx (boot timestamp)\n", (unsigned long long)boot->tod_boot);
    }

    if (boot->module_count > 0) {
        printk("sys: modules: %u loaded\n", boot->module_count);
        for (uint32_t i = 0; i < boot->module_count; i++) {
            printk("     mod[%u] %s (phys=0x%llx, size=%llu bytes)\n",
                   i,
                   boot->modules[i].name,
                   (unsigned long long)boot->modules[i].phys_start,
                   (unsigned long long)boot->modules[i].size_bytes);
        }
    }

    zx_cpu_features_init(boot);
    
    rcu_init();

    pmm_init(boot);

    // mmu_init() reconstructs the kernel page-table handle from the live CR1
    // value set by the bootloader.  It must run after pmm_init() (which
    // places mem_map in HHDM) and before vmm_init() (which needs the handle).
    mmu_init();

    // vmm_init() sets up the kernel address space descriptor and prepares
    // the vmalloc bump allocator.  Must precede slab_init() because the
    // slab calls vmm_notify_slab_ready() to transition the VMA pool.
    vmm_init();

    slab_init();
    kmalloc_init();

    // --- Subsystem Self-Tests ---
    printk("sys: running memory subsystem self-tests...\n");

    // 1. PMM Test
    zx_page_t *p = pmm_alloc_pages(2, ZX_GFP_NORMAL);
    if (!p) panic("test: pmm_alloc_pages failed");
    uint64_t phys = pmm_page_to_phys(p);
    if (phys & 0x3FFF) panic("test: pmm order-2 alignment failed");
    pmm_free_pages(p, 2);
    printk("  [OK] PMM order-2 allocation and alignment.\n");

    // 2. Slab / Kmalloc Test
    void *kptr = kmalloc(128);
    if (!kptr) panic("test: kmalloc failed");
    memset(kptr, 0xAA, 128);
    kfree(kptr);
    printk("  [OK] Slab/kmalloc 128-byte allocation.\n");

    // 3. Large Object Heap (VMM backed) Test
    void *hptr = kheap_zalloc(32768); // 32 KB, spans 8 pages
    if (!hptr) panic("test: kheap_zalloc failed");
    if (((uint64_t)hptr) < VMALLOC_START) panic("test: kheap returned non-vmalloc ptr");
    // Verify zeroed
    for (int i = 0; i < 32768; i++) {
        if (((uint8_t*)hptr)[i] != 0) panic("test: kheap_zalloc not zeroed");
    }
    kheap_free(hptr);
    printk("  [OK] Heap/VMM 32 KB allocation and mapping.\n");

    printk("sys: core.zxfoundation.nucleus initialization complete\n");

    while (true) {
        __asm__ volatile("nop");
    }
}
