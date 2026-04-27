// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sys/panic.c

#include <zxfoundation/panic.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>
#include <arch/s390x/trap/trap.h>

#include <stdarg.h>

// ---------------------------------------------------------------------------
// panic_halt - enter a disabled-wait state.
//
// The PSW address is set to CONFIG_PANIC_HALT_ADDR so the halt is
// distinguishable from a normal end-of-kernel disabled wait in QEMU logs
// and operator consoles.  Safe to call before the console is up.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// panic_emit - emit the panic banner and formatted message via vprintk.
// ---------------------------------------------------------------------------
static void panic_emit(const char *fmt, va_list ap) {
    printk("\nZXFoundation panic\n*** STOP: ");
    vprintk(fmt, ap);
}

// ---------------------------------------------------------------------------
// panic - print a message and halt.
// ---------------------------------------------------------------------------
[[noreturn]] void panic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    panic_emit(fmt, ap);
    va_end(ap);
    panic_halt();
}

// ---------------------------------------------------------------------------
// panic_with_regs - print a register dump, then panic.
// ---------------------------------------------------------------------------
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
