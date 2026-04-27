/// SPDX-License-Identifier: Apache-2.0
/// main.c ZXFoundation kernel entry point

#include <drivers/console/sclp.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>

[[noreturn]] void zxfoundation_global_initialize(void) {
    printk_initialize(sclp_putc);
    printk("ZXFoundation " CONFIG_ULTRASPARK_RELEASE "\n");

    for (;;) {
        __asm__ volatile("nop");
    }
}
