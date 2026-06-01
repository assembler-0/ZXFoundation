/// SPDX-License-Identifier: Apache-2.0
/// @file rcu.h
/// @brief Minimal Read-Copy-Update (RCU).

#pragma once

#include <arch/s390x/cpu/atomic.h>
#include <zxfoundation/sys/preempt.h>

/// @brief RCU callback node.
///
/// Objects that need to be freed after a grace period must embed this structure.
typedef struct rcu_head {
    struct rcu_head *next;                  ///< Linkage into the global callback list.
    void (*func)(struct rcu_head *head);    ///< Callback function.
} rcu_head_t;

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

/// @brief Safely publish a pointer to a newly initialized object.
/// @param[out] p The pointer to update.
/// @param[in]  v The new value.
/// @note The smp_mb() ensures all stores to *v are visible before
///       the pointer itself becomes visible to readers.
#define rcu_assign_pointer(p, v)    \
    do {                            \
        smp_mb();                   \
        (p) = (v);                  \
    } while (0)

/// @brief Safely read an RCU-protected pointer.
/// @param[in] p The pointer to read.
/// @return The value of the pointer.
/// @note The compiler barrier prevents the compiler from re-reading the
///       pointer after it has been dereferenced.
#define rcu_dereference(p)          \
    ({                              \
        __typeof__(p) _v = (p);     \
        barrier();                  \
        _v;                         \
    })

/// @brief Register a callback to be called after the next grace period.
/// @param[in,out] head RCU head embedded in the object to be freed.
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
