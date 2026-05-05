// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sys/panic.c

#include <arch/s390x/cpu/processor.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>

static void panic_emit(const char *fmt, va_list ap) {
    printk("\nZXFoundation panic\n*** STOP: ");
    vprintk(fmt, ap);
    printk("\n");
}

[[noreturn]] void panic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    panic_emit(fmt, ap);
    va_end(ap);
    arch_sys_halt();
}

