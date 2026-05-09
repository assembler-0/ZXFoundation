// SPDX-License-Identifier: Apache-2.0
// arch/s390x/trap/ext.c
//
/// @brief External interrupt handler for z/Architecture.
///
///        CPU timer (0x1004) and clock comparator (0x1005) are intercepted
///        before the generic irq_dispatch() path because their ext_code
///        values exceed the irqdesc table range (ZX_IRQ_NR_MAX = 0x0400).
///        Routing them through the table would require a 64 KB array for
///        the full 16-bit ext_code space — wasteful for a sparse set.
///        All other EXT codes are dispatched via the generic table.

#include <arch/s390x/cpu/irq_frame.h>
#include <arch/s390x/cpu/lowcore.h>
#include <zxfoundation/sys/irq/irqdesc.h>
#include <zxfoundation/time/ktime.h>

/// EXT subclass codes for the two time-critical interrupts.
#define EXT_CPU_TIMER           0x1004U
#define EXT_CLOCK_COMPARATOR    0x1005U

/// @brief External interrupt C handler — called from trap_ext_entry.
/// @param frame  Interrupt frame built by entry.S on the async stack.
void do_ext_interrupt(zx_irq_frame_t *frame) {
    zx_lowcore_t *lc = zx_lowcore();

    const uint16_t ext_code = lc->ext_int_code;

    // Hot path: CPU timer and clock comparator bypass the generic table.
    if (ext_code == EXT_CPU_TIMER) {
        time_cpu_timer_handler();
        return;
    }
    if (ext_code == EXT_CLOCK_COMPARATOR) {
        time_clock_comparator_handler();
        return;
    }

    const uint16_t irq = (uint16_t)(ZX_IRQ_BASE_EXT + ext_code);
    irq_dispatch(irq, frame);
}
