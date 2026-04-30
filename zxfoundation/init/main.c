/// SPDX-License-Identifier: Apache-2.0
/// main.c ZXFoundation kernel entry point

#include <drivers/console/diag.h>
#include <drivers/console/sclp.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>

/// @brief initialization entry point
[[noreturn]] void zxfoundation_global_initialize(void) {
    diag_setup();
    printk_initialize(diag_putc);
    printk("ZXFoundation %s copyright (C) 2026 assembler-0 All rights reserved.\n", CONFIG_ULTRASPARK_RELEASE);

    if (sclp_setup() == 0) {
        printk("SCLP console initialized\n");
    } else {
        printk("SCLP console setup failed\n");
    }

    while (true) {
        __asm__ volatile("nop");
    }
}
