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
#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/zxvl_private.h>
#include <arch/s390x/init/zxfl/lowcore.h>
#include <arch/s390x/cpu/features.h>
#include <arch/s390x/mmu.h>
#include <drivers/console/diag.h>
#include <crypto/sha256.h>
#include <lib/string.h>

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

/// @brief Re-verify kernel segment checksums from the HHDM-mapped image.
static void verify_kernel_checksums(const zxfl_boot_protocol_t *boot) {
    const uint64_t virt_base = boot->kernel_phys_start + CONFIG_KERNEL_VIRT_OFFSET;
    const zxvl_checksum_table_t *tbl =
        (const zxvl_checksum_table_t *)(uintptr_t)(virt_base + ZXVL_CKSUM_TABLE_OFFSET);

    if (tbl->magic != ZXVL_CKSUM_MAGIC) {
        printk("sys: WARNING — no kernel checksum table, integrity unverified\n");
        return;
    }
    if (tbl->version != ZXVL_CKSUM_VERSION || tbl->count == 0 ||
        tbl->count > ZXVL_CKSUM_MAX_ENTRIES)
        panic("sys: kernel checksum table corrupt");

    const uint32_t algo = tbl->algo;
    if (algo != ZXVL_CKSUM_ALGO_SHA256)
        panic("sys: kernel checksum algorithm unsupported");

    for (uint32_t i = 0; i < tbl->count; i++) {
        const zxvl_cksum_entry_t *e = &tbl->entries[i];
        if (e->size == 0) continue;
        const uint64_t virt_seg = e->phys_start + CONFIG_KERNEL_VIRT_OFFSET;
        uint8_t actual[ZXFL_SHA256_DIGEST_SIZE];
        zxfl_sha256((const void *)(uintptr_t)virt_seg, (size_t)e->size, actual);
        if (memcmp(actual, e->digest, ZXFL_SHA256_DIGEST_SIZE) != 0)
            panic("sys: kernel segment checksum mismatch — image tampered");
    }
    printk("sys: kernel checksums verified (%u segments)\n", tbl->count);
}

[[noreturn]] void zxfoundation_global_initialize(zxfl_boot_protocol_t *boot) {
    zxfl_lowcore_setup();

    diag_setup();
    printk_initialize(diag_putc);
    printk("sys: ZXFoundation (R) %s CONFIDENTIAL - copyright (C) 2026 assembler-0 all rights reserved.\n",
           CONFIG_ULTRASPARK_RELEASE);

    if (!boot || boot->magic != ZXFL_MAGIC)
        panic("sys: protocol missing or corrupt");

    const uint64_t expected = ZXVL_COMPUTE_TOKEN(boot->stfle_fac[0], boot->ipl_schid);
    if (boot->binding_token != expected)
        panic("sys: binding token mismatch — unauthorized loader");

    validate_stack_frame(boot);

    verify_kernel_checksums(boot);

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

    arch_cpu_features_init(boot);
    rcu_init();

    pmm_init(boot);
    mmu_init();
    vmm_init();

    slab_init();
    kmalloc_init();

    printk("sys: core.zxfoundation.nucleus initialization complete\n");

    while (true) {
        __asm__ volatile("nop");
    }
}
