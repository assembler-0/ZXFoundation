// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sys/syschk.c
//
/// @brief ZXFoundation System Check implementation.
///
///        HALT PATH
///        =========
///        1. Disable external interrupts on the issuing CPU (PSW mask).
///        2. Atomically mark the system as halting so re-entrant syschks
///           from other CPUs do not attempt a second teardown.
///        3. For FATAL/CRITICAL: issue SIGP STOP to every other CPU.
///           sigp_busy() retries on CC=2; CC=3 (not operational) is
///           silently skipped — a stopped CPU cannot corrupt state.
///        4. Emit the formatted message via printk.
///        5. Load the disabled-wait PSW via arch_sys_halt().
///
///        RE-ENTRANCY GUARD
///        =================
///        g_halting is a plain volatile int used as a flag.  A full atomic
///        CAS is intentionally avoided: if the memory subsystem is corrupt
///        we cannot trust atomic operations.  The flag is written once with
///        a volatile store; a second syschk on any CPU will see it set and
///        skip straight to arch_sys_halt() without attempting another
///        teardown.

#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>
#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/cpu/irq.h>
#include <arch/s390x/cpu/smp.h>
#include <lib/vsprintf.h>

static volatile int          g_halting;
static zx_syschk_filter_fn   g_filter;

static const char *class_name(const zx_syschk_code_t code) {
    switch (ZX_SYSCHK_CLASS(code)) {
        case ZX_SYSCHK_CLASS_FATAL:    return "fatal";
        case ZX_SYSCHK_CLASS_CRITICAL: return "critical";
        case ZX_SYSCHK_CLASS_WARNING:  return "warning";
        default:                       return "unknown";
    }
}

static const char *domain_name(const zx_syschk_code_t code) {
    switch (ZX_SYSCHK_DOMAIN(code)) {
        case ZX_SYSCHK_DOMAIN_CORE:  return "core";
        case ZX_SYSCHK_DOMAIN_MEM:   return "mem";
        case ZX_SYSCHK_DOMAIN_SYNC:  return "sync";
        case ZX_SYSCHK_DOMAIN_ARCH:  return "arch";
        case ZX_SYSCHK_DOMAIN_SCHED: return "sched";
        case ZX_SYSCHK_DOMAIN_IO:    return "io";
        default:                     return "unknown";
    }
}

void zx_system_check_set_filter(const zx_syschk_filter_fn fn) {
    g_filter = fn;
}

void zx_system_check(const zx_syschk_code_t code,
                             const char *fmt, ...) {
    const irqflags_t saved_flags = arch_local_save_flags();
    arch_local_irq_disable();

    const unsigned int cls = ZX_SYSCHK_CLASS(code);

    if (cls == ZX_SYSCHK_CLASS_WARNING && g_filter) {
        char msg[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        if (g_filter(code, msg) == ZX_SYSCHK_SUPPRESS) {
            printk("syschk: warning [%s/%s/0x%02x] suppressed by filter: %s\n",
                   class_name(code), domain_name(code),
                   ZX_SYSCHK_TYPE(code), msg);
            arch_local_irq_restore(saved_flags);
            return;
        }
    }

    if (g_halting) {
        arch_sys_halt();
    }
    g_halting = 1;
    g_filter = nullptr;
    printk("\nsyschk: system check [%s/%s/0x%02x]\n",
           class_name(code), domain_name(code), ZX_SYSCHK_TYPE(code));

    {
        char msg[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        printk("syschk: reason: %s\n", msg);
    }

    smp_teardown();

    printk("syschk: smp teardown complete — entering disabled-wait state\n");

    arch_sys_halt();
}
