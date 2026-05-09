// SPDX-License-Identifier: Apache-2.0
// zxfoundation/time/ktime.c
//
/// @brief Kernel time subsystem — ktime_get(), ktime_sleep(), time_init().
///
///        DESIGN
///        ======
///        ktime_get() reads STCKF (via tod_read()) and subtracts the boot
///        offset recorded at time_init().  The result is converted from TOD
///        units to nanoseconds using the exact rational 125/512.
///
///        ktime_sleep() programs the clock comparator for the target absolute
///        TOD value and spins (with arch_cpu_relax()) until the TOD clock
///        passes the deadline.  This is a busy-wait — acceptable for early
///        boot and short delays.  Once the scheduler exists, this will be
///        replaced with a proper block/wake implementation.
///
///        EXT INTERRUPT DISPATCH
///        ======================
///        The irqdesc table has ZX_IRQ_NR_MAX = 0x0400 entries.  The EXT
///        dispatch in ext.c computes irq = ZX_IRQ_BASE_EXT + ext_code.
///        For CPU timer (ext_code = 0x1004) that gives 0x1104, which is
///        out of range.  Rather than expanding the table (which would waste
///        memory for the sparse 16-bit ext_code space), we intercept the
///        two hot EXT codes directly in do_ext_interrupt() before the
///        generic irq_dispatch() call.  This is the same approach used by
///        Linux for the s390 CPU timer and clock comparator.

#include <zxfoundation/time/ktime.h>
#include <zxfoundation/time/timer.h>
#include <arch/s390x/time/tod.h>
#include <arch/s390x/cpu/processor.h>

ktime_t ktime_get(void) {
    uint64_t tod = tod_read();
    uint64_t delta = tod - tod_boot_offset();
    return delta * 125 / 512;
}

/// @brief Called from do_ext_interrupt() when ext_code == 0x1004.
void time_cpu_timer_handler(void) {
    tod_cpu_timer_set(-(int64_t)TOD_10MS_IN_TOD);
}

/// @brief Called from do_ext_interrupt() when ext_code == 0x1005.
///        Advances the per-CPU timer wheel and reprograms the comparator
///        for the next pending timer.
void time_clock_comparator_handler(void) {
    uint64_t now = tod_read();

    // Advance the wheel — fires all expired timers.
    timer_wheel_advance(now);

    // Reprogram the comparator for the next pending timer.
    uint64_t next = timer_wheel_next_expiry();
    if (next != UINT64_MAX)
        tod_clock_comparator_set(next);
    else
        tod_clock_comparator_set(now + TOD_1S_IN_TOD);
}

void ktime_sleep(uint64_t ns) {
    uint64_t deadline = tod_read() + ktime_ns_to_tod(ns);
    tod_clock_comparator_set(deadline);
    while (tod_read() < deadline)
        arch_cpu_relax();
}

static void time_init_cpu(void) {
    timer_wheel_init();

    tod_enable_ext_interrupts();
    tod_cpu_timer_set(-(int64_t)TOD_10MS_IN_TOD);

    tod_clock_comparator_set(tod_read() + TOD_1S_IN_TOD);
}

void time_init(uint64_t boot_tod) {
    tod_set_boot_offset(boot_tod ? boot_tod : tod_read());
    time_init_cpu();
}

void time_init_ap(void) {
    time_init_cpu();
}
