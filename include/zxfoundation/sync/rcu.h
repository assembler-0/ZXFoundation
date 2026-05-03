// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/rcu.h
//
/// @brief Minimal Read-Copy-Update (RCU) for a non-preemptive kernel.
///
///        DESIGN
///        ======
///        In a non-preemptive kernel, a grace period is simply the
///        interval during which every CPU has passed through at least one
///        point where it is not in an RCU read-side critical section.
///        Since there is no preemption, any CPU that is not in rcu_read_lock()
///        is implicitly outside the read-side critical section.
///
///        This implementation uses a global generation counter.
///        synchronize_rcu() increments the generation and then waits
///        (spinning) until all CPUs have observed the new generation.
///        For a single-CPU kernel this degenerates to a pair of barriers.
///
///        rcu_assign_pointer / rcu_dereference are the only primitives
///        needed by callers.  The barrier semantics are:
///          - rcu_assign_pointer: smp_mb() before the store so all prior
///            writes to the new object are visible before the pointer
///            becomes visible.
///          - rcu_dereference: compiler barrier (data dependency on s390x
///            TSO is sufficient; no hardware fence needed for the load).
///
///        CALLBACKS
///        =========
///        call_rcu() registers a callback to be invoked after the next
///        grace period.  The callback list is flushed by synchronize_rcu().
///        This is a simple linked list — no batching yet.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/atomic.h>

/// @brief RCU callback node.  Embed in the object being freed.
typedef struct rcu_head {
    struct rcu_head *next;
    void (*func)(struct rcu_head *head);
} rcu_head_t;

/// @brief Enter an RCU read-side critical section.
///        On a non-preemptive kernel this is a compiler barrier only —
///        it prevents the compiler from moving loads out of the section.
static inline void rcu_read_lock(void) {
    __asm__ volatile ("" ::: "memory");
}

/// @brief Exit an RCU read-side critical section.
static inline void rcu_read_unlock(void) {
    __asm__ volatile ("" ::: "memory");
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
