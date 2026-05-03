// SPDX-License-Identifier: Apache-2.0
// zxfoundation/init/main.c — ZXFoundation kernel entry point

#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/slab.h>
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

    zx_cpu_features_init(boot);
    
    zxfl_lowcore_setup();
    
    rcu_init();

    pmm_init(boot);

    slab_init();

    printk("sys: core.zxfoundation.nucleus init complete\n");

    while (true) {
        __asm__ volatile("nop");
    }
}
