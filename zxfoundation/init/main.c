// SPDX-License-Identifier: Apache-2.0
// zxfoundation/init/main.c — ZXFoundation kernel entry point

#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zxconfig.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/vmm.h>
#include <zxfoundation/memory/slab.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/object/koms.h>
#include <zxfoundation/sys/irq/irqdesc.h>
#include <zxfoundation/time/ktime.h>
#include <zxfoundation/sys/simplelog.h>
#include <zxfoundation/cmdlineopts.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/zxvl.h>
#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/cpu/features.h>
#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/mmu/mmu.h>
#include <arch/s390x/cpu/smp.h>
#include <drivers/console/diag.h>
#include <crypto/sha256.h>
#include <lib/string.h>
#include <lib/cmdline.h>

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
    zx_system_check(ZX_SYSCHK_CORE_CORRUPT, "sys: stack frame corruption/not found — unauthorized loader");
}

/// @brief Re-verify kernel segment checksums from the HHDM-mapped image.
static void verify_kernel_checksums(const zxfl_boot_protocol_t *boot) {
    if (!boot->cksum_table_phys) {
        zx_system_check(ZX_SYSCHK_CORE_CORRUPT, "sys: no kernel checksum table, integrity unverified — unauthorized loader");
    }
    const zxvl_checksum_table_t *tbl =
        (const zxvl_checksum_table_t *)(uintptr_t)boot->cksum_table_phys;
    if (tbl->version != ZXVL_CKSUM_VERSION || tbl->count == 0 ||
        tbl->count > ZXVL_CKSUM_MAX_ENTRIES)
        zx_system_check(ZX_SYSCHK_CORE_CORRUPT, "sys: kernel checksum table corrupt");

    const uint32_t algo = tbl->algo;
    if (algo != ZXVL_CKSUM_ALGO_SHA256)
        zx_system_check(ZX_SYSCHK_CORE_CORRUPT, "sys: kernel checksum algorithm unsupported");

    for (uint32_t i = 0; i < tbl->count; i++) {
        const zxvl_cksum_entry_t *e = &tbl->entries[i];
        if (e->size == 0) continue;
        const uint64_t virt_seg = e->phys_start + CONFIG_KERNEL_VIRT_OFFSET;
        uint8_t actual[ZXFL_SHA256_DIGEST_SIZE];
        zxfl_sha256((const void *)(uintptr_t)virt_seg, (size_t)e->size, actual);
        if (memcmp(actual, e->digest, ZXFL_SHA256_DIGEST_SIZE) != 0)
            zx_system_check(ZX_SYSCHK_CORE_CORRUPT, "sys: kernel segment checksum mismatch — image tampered");
    }
    printk(ZX_INFO "sys: kernel checksums verified (%u segments)\n", tbl->count);
}

static void verify_protocol_integrity(zxfl_boot_protocol_t *boot) {
    if (boot->magic != ZXFL_MAGIC)
        zx_system_check(ZX_SYSCHK_CORE_CORRUPT, "sys: protocol missing or corrupt");

    const uint64_t expected = ZXVL_COMPUTE_TOKEN(boot->stfle_fac[0], boot->ipl_schid);
    if (boot->binding_token != expected)
        zx_system_check(ZX_SYSCHK_CORE_CORRUPT, "sys: binding token mismatch — unauthorized loader");
}

static void dump_machine_info(zxfl_boot_protocol_t *boot) {
    if (boot->flags & ZXFL_FLAG_SYSINFO) {
        printk(ZX_DEBUG "machine: %s %s model %s (s/n %s) plant %s\n",
               boot->sysinfo.manufacturer,
               boot->sysinfo.type,
               boot->sysinfo.model,
               boot->sysinfo.sequence,
               boot->sysinfo.plant);
        printk(ZX_DEBUG "lpar: %s (id %u)\n",
               boot->sysinfo.lpar_name[0] ? boot->sysinfo.lpar_name : "<bare-metal>",
               boot->sysinfo.lpar_number);
        printk(ZX_DEBUG "cpus: %u total, %u configured, %u standby (rating %u)\n",
               boot->sysinfo.cpus_total,
               boot->sysinfo.cpus_configured,
               boot->sysinfo.cpus_standby,
               boot->sysinfo.capability);
    }

    struct s390x_cpuid id;
    arch_get_cpu_id(&id);
    printk(ZX_DEBUG "cpuid: machine %d version %d\n",
           id.machine, id.version);

    if (boot->flags & ZXFL_FLAG_SMP) {
        printk(ZX_DEBUG "smp: %u processors detected\n", boot->cpu_count);
        for (uint32_t i = 0; i < boot->cpu_count; i++) {
            printk(ZX_DEBUG "     cpu[%u] type=%s addr=0x%04x state=%u bsp=%s numa=%u topology=[drawer=%u, book=%u, socket=%u, chip=%u, thread=%u]\n",
                   i,
                   arch_cpu_type_to_string(boot->cpu_map[i].type),
                   boot->cpu_map[i].cpu_addr,
                   boot->cpu_map[i].state,
                   boot->cpu_map[i].cpu_addr == boot->bsp_cpu_addr ? "yes" : "no",
                   boot->cpu_map[i].numa_node,
                   boot->cpu_map[i].drawer_id,
                   boot->cpu_map[i].book_id,
                   boot->cpu_map[i].socket_id,
                   boot->cpu_map[i].chip_id,
                   boot->cpu_map[i].thread_id);
        }
    }

    if (boot->flags & ZXFL_FLAG_TOD) {
        printk(ZX_DEBUG "tod: 0x%016llx (zxfl)\n", (unsigned long long)boot->tod_boot);
    }

    if (boot->module_count > 0) {
        printk(ZX_DEBUG "modules: %u loaded\n", boot->module_count);
        for (uint32_t i = 0; i < boot->module_count; i++) {
            printk(ZX_DEBUG "     mod[%u] %s (phys=0x%llx, size=%llu bytes)\n",
                   i,
                   boot->modules[i].name,
                   (unsigned long long)boot->modules[i].phys_start,
                   (unsigned long long)boot->modules[i].size_bytes);
        }
    }

    // Print NUMA memory region summary from the loader memory map.
    if (boot->flags & ZXFL_FLAG_MEM_MAP) {
        const zxfl_mem_region_t *map =
            (const zxfl_mem_region_t *)(uintptr_t)boot->mem_map_addr;
        printk(ZX_DEBUG "mem: %u regions in boot memory map\n", boot->mem_map_count);
        for (uint32_t i = 0; i < boot->mem_map_count; i++) {
            const zxfl_mem_region_t *r = &map[i];
            if (r->type != ZXFL_MEM_USABLE) continue;
            printk(ZX_DEBUG "     region[%u] base=0x%llx len=%llu MB numa=%u\n",
                   i,
                   (unsigned long long)r->base,
                   (unsigned long long)(r->length / (1024 * 1024)),
                   r->numa_node);
        }
    }
}

#define if_cmdline_has(boot, option)                                    \
    if (cmdline_find_option_bool((const char *)boot->cmdline_addr,      \
                                 (signed)boot->cmdline_len, option))

#define if_cmdline_not_has(boot, option)                                \
    if (!cmdline_find_option_bool((const char *)boot->cmdline_addr,     \
                                 (signed)boot->cmdline_len, option))


[[noreturn]] void zxfoundation_global_initialize(zxfl_boot_protocol_t *boot) {
    if (!boot)
        arch_sys_halt();

    percpu_init_bsp();
    zx_lowcore_setup_bsp();
    zx_lowcore()->kernel_stack = boot->kernel_stack_top;
    zx_syschk_initialize(boot);

    diag_setup();
    printk_initialize(diag_putc);
    simplelog_initialize(diag_putc);

    time_init((boot->flags & ZXFL_FLAG_TOD) ? boot->tod_boot : 0);

    printk(ZX_INFO "sys: ZXFoundation (TM) %s - copyright (C) 2026 assembler-0 all rights reserved.\n",
           CONFIG_ZX_RELEASE);

    printk(ZX_INFO "cmdline: %s\n", (const char *)boot->cmdline_addr);

    if_cmdline_not_has(boot, CMDLINE_SKIP_ZXVL_CHECK) {
        verify_protocol_integrity(boot);
        validate_stack_frame(boot);
        verify_kernel_checksums(boot);
    } else {
        printk(ZX_WARN "sys: skipping ZXVerifiedLoad integrity checks (due to '%s')", CMDLINE_SKIP_ZXVL_CHECK);
    }

    dump_machine_info(boot);

    arch_cpu_features_init(boot->stfle_fac, boot->stfle_count);
    rcu_init();

    pmm_init(boot);
    smp_prepare_aps(boot);

    pmm_pcplist_init(0);

    mmu_init();
    pmm_verify_hhdm(boot);
    vmm_init();

    slab_init();
    kmalloc_init();

    koms_init();
    irq_subsystem_init();

    smp_start_aps();

    printk(ZX_INFO "sys: core.zxfoundation.nucleus initialization complete\n");

    while (true) arch_cpu_relax();
}
