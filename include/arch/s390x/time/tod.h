// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/time/tod.h
//
/// @brief z/Architecture TOD clock and CPU timer primitives.

#pragma once

#include <zxfoundation/types.h>

/// TOD clock frequency: 2^12 ticks per microsecond.
#define TOD_1MS_IN_TOD      4096000ULL          ///< 1 ms in TOD units.
#define TOD_10MS_IN_TOD     40960000ULL         ///< 10 ms in TOD units.
#define TOD_1S_IN_TOD       4096000000ULL       ///< 1 s in TOD units.

/// @brief Read the TOD clock without serialization (STCKF).
///        Safe from any context including hard-IRQ.
/// @return Raw 64-bit TOD value.
uint64_t tod_read(void);

/// @brief Read the CPU timer (STPT).
/// @return Current CPU timer value (signed, counts up from negative).
int64_t tod_cpu_timer_read(void);

/// @brief Load the CPU timer (SPT) with a negative countdown value.
/// @param value  Negative TOD-unit countdown (e.g. -TOD_10MS_IN_TOD).
void tod_cpu_timer_set(int64_t value);

/// @brief Read the clock comparator (STCKC).
/// @return Absolute TOD value at which the next EXT 0x1005 fires.
uint64_t tod_clock_comparator_read(void);

/// @brief Set the clock comparator (SCKC) to an absolute TOD value.
/// @param abs_tod  Absolute TOD value.
void tod_clock_comparator_set(uint64_t abs_tod);

/// @brief Enable the CPU timer and clock comparator external interrupt masks
///        in CR0.  Must be called on every CPU (BSP + each AP) after the
///        interrupt handler PSWs are installed.
void tod_enable_ext_interrupts(void);

/// @brief Record the TOD value at kernel boot.  Called once from time_init()
///        on the BSP before any ktime_get() call.
/// @param boot_tod  Raw TOD value read immediately at boot.
void tod_set_boot_offset(uint64_t boot_tod);

/// @brief Return the boot-time TOD offset recorded by tod_set_boot_offset().
uint64_t tod_boot_offset(void);
