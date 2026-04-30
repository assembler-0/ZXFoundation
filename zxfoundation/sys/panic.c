// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sys/panic.c

#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <arch/s390x/trap/trap.h>

[[noreturn]] void panic_halt(void) {
    __asm__ volatile(
        "   larl    %%r1, 1f\n"
        "   lpswe   0(%%r1)\n"
        "   .align  8\n"
        "1: .quad   %0, %1\n"
        :
        : "i"(CONFIG_PSW_DISABLED_WAIT),
          "i"(CONFIG_PANIC_HALT_ADDR)
        : "r1", "memory"
    );
    __builtin_unreachable();
}

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
    panic_halt();
}

[[noreturn]] void panic_with_regs(const pt_regs_t *regs, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    panic_emit(fmt, ap);
    va_end(ap);

    if (regs) {
        printk("Register dump:\n");
        printk("  PSW  mask=%016lx  addr=%016lx\n",
               regs->psw_mask, regs->psw_addr);
        for (int i = 0; i < 16; i += 2) {
            printk("  r%-2d  %016lx    r%-2d  %016lx\n",
                   i,     regs->gprs[i],
                   i + 1, regs->gprs[i + 1]);
        }
    }

    panic_halt();
}
