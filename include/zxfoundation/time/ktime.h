// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/time/ktime.h
//
/// @brief Kernel time type and core time API.

#pragma once

#include <zxfoundation/types.h>

/// @brief Kernel time in nanoseconds since boot.
///        Monotonic, never wraps in any practical timeframe.
typedef uint64_t ktime_t;

/// @brief Read the current kernel time.
///        Callable from any context (hard-IRQ, softirq, process).
///        No lock, no sleep.
/// @return Nanoseconds since kernel boot.
ktime_t ktime_get(void);

/// @brief Convert nanoseconds to TOD units.
///        Inverse of the ktime conversion: tod = ns * 512 / 125.
/// @param ns  Duration in nanoseconds.
/// @return Duration in TOD units.
static inline uint64_t ktime_ns_to_tod(uint64_t ns) {
    // 1 ns = 4096/1000 TOD units = 512/125 TOD units.
    return ns * 512 / 125;
}

/// @brief Convert TOD units to nanoseconds.
/// @param tod  Duration in TOD units.
/// @return Duration in nanoseconds.
static inline uint64_t ktime_tod_to_ns(uint64_t tod) {
    // 1 TOD unit = 1000/4096 ns = 125/512 ns.
    return tod * 125 / 512;
}

/// @brief Called from do_ext_interrupt() when ext_code == 0x1004.
void time_cpu_timer_handler(void);

/// @brief Called from do_ext_interrupt() when ext_code == 0x1005.
///        Advances the per-CPU timer wheel and reprograms the comparator
///        for the next pending timer.
void time_clock_comparator_handler(void);

/// @brief Initialize the time subsystem.
///        Uses the loader-provided TOD value as the boot epoch so that
///        ktime_get() reports time since IPL, not since time_init().
///        Falls back to tod_read() if the loader did not set ZXFL_FLAG_TOD.
/// @param boot_tod  Loader-recorded TOD value (boot->tod_boot), or 0 to
///                  read the current TOD clock as the epoch.
void time_init(uint64_t boot_tod);

/// @brief Initialize the time subsystem on an AP.
///        Enables CPU timer and clock comparator interrupts on the calling
///        CPU.  Called from ap_startup() on each AP after SMP bringup.
void time_init_ap(void);

/// @brief Sleep for at least @p ns nanoseconds.
///        Blocks the calling thread using the clock comparator.
///        Must NOT be called from hard-IRQ or softirq context.
/// @param ns  Duration in nanoseconds.
void ktime_sleep(uint64_t ns);
