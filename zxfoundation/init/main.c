/// SPDX-License-Identifier: Apache-2.0
/// main.c ZXFoundation kernel entry point

#include <arch/s390x/trap/trap.h>
#include <drivers/console/diag.h>
#include <drivers/console/sclp.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>

[[noreturn]] void zxfoundation_global_initialize(void) {
    diag_setup();
    printk_initialize(diag_putc);
    printk("ZXFoundation %s copyright (C) 2026 assembler-0 All rights reserved.", CONFIG_ULTRASPARK_RELEASE);

    panic("end of kernel");
}
