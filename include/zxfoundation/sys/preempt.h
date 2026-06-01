/// SPDX-License-Identifier: Apache-2.0
/// @file preempt.h
/// @brief Core preemption control primitives.

#pragma once

#include <zxfoundation/common.h>
#include <arch/s390x/cpu/lowcore.h>

/// @brief Read the current preemption count.
static inline int32_t preempt_count(void) {
    return read_once(zx_lowcore()->preempt_count);
}

/// @brief Disable preemption.
///        Uses __atomic_add_fetch with __ATOMIC_RELAXED to emit a single
///        instruction (like `asi`) that is safe against interrupts on the
///        same CPU. The compiler barrier prevents hoisting code out of
///        the preempt-disabled section.
static inline void preempt_disable(void) {
    __atomic_add_fetch(&zx_lowcore()->preempt_count, 1, __ATOMIC_RELAXED);
    barrier();
}

/// @brief Enable preemption without checking for reschedules.
static inline void preempt_enable_no_resched(void) {
    barrier();
    __atomic_sub_fetch(&zx_lowcore()->preempt_count, 1, __ATOMIC_RELAXED);
}

/// @brief Hook to call the scheduler. To be implemented when the scheduler
///        is added.
extern void preempt_schedule(void);

/// @brief Enable preemption. If the count reaches zero and a reschedule
///        was requested (e.g. by an interrupt that woke a higher-priority
///        task), we call the scheduler.
static inline void preempt_enable(void) {
    barrier();
    if (unlikely(__atomic_sub_fetch(&zx_lowcore()->preempt_count, 1, __ATOMIC_RELAXED) == 0)) {
        // TIF_NEED_RESCHED equivalent will be checked here.
        // For now, if softirqs are pending, we might process them, but
        // true preemption scheduling will check a need_resched flag.
        // If need_resched is true: preempt_schedule();
        // TODO: This is a placeholder. Implement proper rescheduling logic.
    }
}
