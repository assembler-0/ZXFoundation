/// SPDX-License-Identifier: Apache-2.0
/// @file rcu.h
/// @brief Minimal Read-Copy-Update (RCU).

#pragma once

#include <arch/s390x/cpu/atomic.h>
#include <zxfoundation/sync/rcu_types.h>
#include <zxfoundation/sys/preempt.h>

/// @brief Enter an RCU read-side critical section.
/// @note In this non-preemptive implementation, this is currently a compiler
///       barrier. Readers must not sleep or block within the critical section.
static inline void rcu_read_lock(void) {
    preempt_disable();
}

/// @brief Exit an RCU read-side critical section.
static inline void rcu_read_unlock(void) {
    preempt_enable();
}

/// @brief Register a callback to be called after the next grace period.
/// @param[in]     func Callback function to invoke.
void call_rcu(rcu_head_t *head, void (*func)(rcu_head_t *));

/// @brief Block until all pre-existing RCU read-side critical sections complete.
/// @note After readers complete, all pending call_rcu() callbacks are flushed.
void synchronize_rcu(void);

/// @brief Initialize the RCU subsystem.
void rcu_init(void);

/// @brief Report a quiescent state for the current CPU.
/// @note Must be called from the idle loop and scheduler tick to allow
///       grace periods to progress.
void rcu_report_qs(void);
