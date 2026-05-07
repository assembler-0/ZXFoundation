// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/rcu.h
//
/// @brief Minimal Read-Copy-Update (RCU)

#pragma once

#include <arch/s390x/cpu/atomic.h>

/// @brief RCU callback node.  Embed in the object being freed.
typedef struct rcu_head {
    struct rcu_head *next;
    void (*func)(struct rcu_head *head);
} rcu_head_t;

/// @brief Enter an RCU read-side critical section.
static inline void rcu_read_lock(void) {
    barrier(); // TODO: implement RCU read lock
}

/// @brief Exit an RCU read-side critical section.
static inline void rcu_read_unlock(void) {
    barrier(); // TODO: implement RCU read unlock
}

/// @brief Safely publish a pointer to a newly initialized object.
///        The smp_mb() ensures all stores to *p_new are visible before
///        the pointer itself becomes visible to readers.
#define rcu_assign_pointer(p, v)    \
    do {                            \
        smp_mb();                   \
        (p) = (v);                  \
    } while (0)

/// @brief Safely read an RCU-protected pointer.
///        The compiler barrier prevents the compiler from re-reading the
///        pointer after it has been dereferenced (which would break the
///        data-dependency ordering guarantee).
#define rcu_dereference(p)          \
    ({                              \
        __typeof__(p) _v = (p);     \
        barrier();                  \
        _v;                         \
    })

/// @brief Register a callback to be called after the next grace period.
void call_rcu(rcu_head_t *head, void (*func)(rcu_head_t *));

/// @brief Block until all pre-existing RCU read-side critical sections
///        have completed, then flush all pending call_rcu() callbacks.
void synchronize_rcu(void);

/// @brief Initialize the RCU subsystem.  Called once at kernel startup.
void rcu_init(void);

/// @brief Report a quiescent state for the current CPU.
///        Must be called from the idle loop and any other long-running
///        non-read-side context (e.g. scheduler tick).
void rcu_report_qs(void);
