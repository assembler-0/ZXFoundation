// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/srcu.h
//
/// @brief Sleepable RCU (SRCU) for ZXFoundation.
///
///        DESIGN
///        ======
///        Unlike classic RCU, SRCU allows read-side critical sections to
///        sleep (block on a mutex, wait for I/O, etc.).  Each SRCU domain
///        is an independent srcu_struct with its own grace-period counter
///        and per-CPU read-side counters.
///
///        READ SIDE
///        =========
///        srcu_read_lock() increments the current CPU's read counter for
///        the domain and returns an index (0 or 1) identifying which
///        counter slot was used.  srcu_read_unlock() decrements that slot.
///        Two slots are used alternately so that synchronize_srcu() can
///        wait for the "old" slot to drain while new readers use the "new"
///        slot.
///
///        WRITE SIDE
///        ==========
///        synchronize_srcu() flips the active slot, then waits until the
///        sum of all per-CPU counters for the old slot reaches zero.
///        This may sleep (calls mutex_lock internally) so it must not be
///        called from interrupt context.
///
///        INITIALIZATION
///        ==============
///        DEFINE_SRCU(name)  — static definition
///        srcu_init(s)       — runtime initialization

#pragma once

#include <arch/s390x/cpu/atomic.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/sync/rcu.h>

/// @brief Per-CPU read counters for one SRCU domain.
///        Two slots, indexed by srcu_struct::idx.
typedef struct {
    atomic_t c[2];
} srcu_percpu_t;

/// @brief One SRCU domain.
typedef struct srcu_struct {
    int             idx;                        ///< Active slot (0 or 1).
    srcu_percpu_t   pcpu[MAX_CPUS];             ///< Per-CPU read counters.
    atomic_t        gp_seq;                     ///< Grace-period counter.
} srcu_struct_t;

#define SRCU_INIT(name) {                       \
    .idx    = 0,                                \
    .gp_seq = ATOMIC_INIT(0),                   \
}

#define DEFINE_SRCU(name)   srcu_struct_t name = SRCU_INIT(name)

/// @brief Initialize an SRCU domain at runtime.
void srcu_init(srcu_struct_t *s);

/// @brief Enter an SRCU read-side critical section.
/// @return Index to pass to srcu_read_unlock().
int srcu_read_lock(srcu_struct_t *s);

/// @brief Exit an SRCU read-side critical section.
/// @param idx  Value returned by the matching srcu_read_lock().
void srcu_read_unlock(srcu_struct_t *s, int idx);

/// @brief Wait for all pre-existing SRCU readers to complete.
///        May sleep.  Must not be called from interrupt context.
void synchronize_srcu(srcu_struct_t *s);

/// @brief Register a callback to be called after the next SRCU grace period.
void call_srcu(srcu_struct_t *s, rcu_head_t *head,
               void (*func)(rcu_head_t *));
