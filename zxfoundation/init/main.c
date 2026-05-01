/// SPDX-License-Identifier: Apache-2.0
/// main.c ZXFoundation kernel entry point

#include <drivers/console/diag.h>
#include <drivers/console/sclp.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <zxfoundation/sys/panic.h>
#include <arch/s390x/init/zxfl/zxfl.h>

/// @brief initialization entry point
[[noreturn]] void zxfoundation_global_initialize(zxfl_boot_protocol_t *boot) {
    diag_setup();
    printk_initialize(diag_putc);

    printk("sys: ZXFoundation %s copyright (C) 2026 assembler-0 All rights reserved.\n", CONFIG_ULTRASPARK_RELEASE);
    printk("sys: DIAG 8 console in use\n");

    if (!boot || boot->magic != ZXFL_MAGIC) {
        panic("ZXFoundationLoader corruption");
    }

    if (sclp_setup() == 0) {
        printk("sys: SCLP console initialized\n");
    } else {
        printk("sys: SCLP console setup failed\n");
    }

    while (true) {
        __asm__ volatile("nop");
    }
}
