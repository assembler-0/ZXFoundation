/// SPDX-License-Identifier: Apache-2.0
/// main.c ZXFoundation kernel entry point

#include <drivers/console/sclp.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>

// ---------------------------------------------------------------------------
// zxfoundation_global_initialize - kernel initialization sequence
// - No returns
// ---------------------------------------------------------------------------
[[noreturn]] void zxfoundation_global_initialize(void) {
    sclp_setup();
    printk_initialize(sclp_putc);
    printk("ZXFoundation " CONFIG_ULTRASPARK_RELEASE " for IBM z/Architecture processors\n");
    printk("Copyright (C) 2026 assembler-0\n");

    for (;;) {
        __asm__ volatile("nop");
    }
}
