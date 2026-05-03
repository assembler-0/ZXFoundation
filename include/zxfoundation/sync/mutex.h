// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/mutex.h
//
/// @brief Sleeping mutex (non-recursive, non-IRQ-safe).
///
///        DESIGN
///        ======
///        The mutex is a two-phase lock:
///          1. Fast path: atomic CAS on state (0→1).  No contention → zero
///             overhead beyond the CAS.
///          2. Slow path: the loser enqueues itself on the wait-queue and
///             spins on its wq_entry_t::done flag.  When the holder calls
///             mutex_unlock(), it pops the first waiter and sets its done
///             flag, transferring ownership directly without releasing the
///             lock to the open market (handoff protocol).  This prevents
///             starvation under high contention.
///
///        OWNERSHIP TRACKING
///        ==================
///        owner stores the address of the holder's stack frame (passed by
///        the caller as a cookie).  In a future scheduler integration this
///        becomes the task pointer.  It is used only for deadlock detection
///        in debug builds — not for correctness.
///
///        CONSTRAINTS
///        ===========
///        - Must NOT be acquired from interrupt context (use spinlock_irqsave).
///        - Must NOT be acquired recursively.
///        - mutex_unlock() must be called from the same context as mutex_lock().

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/atomic.h>
#include <zxfoundation/spinlock.h>
#include <zxfoundation/sync/waitqueue.h>

typedef struct {
    atomic_t    state;   ///< 0 = unlocked, 1 = locked (no waiters), 2 = locked (waiters)
    spinlock_t  wait_lock;
    waitqueue_t waiters;
} mutex_t;

#define MUTEX_INIT  { .state = ATOMIC_INIT(0), .wait_lock = SPINLOCK_INIT, \
                      .waiters = WAITQUEUE_INIT }
#define DEFINE_MUTEX(name)  mutex_t name = MUTEX_INIT

/// @brief Runtime initialize a mutex.
static inline void mutex_init(mutex_t *m) {
    atomic_set(&m->state, 0);
    spin_lock_init(&m->wait_lock);
    waitqueue_init(&m->waiters);
}

/// @brief Acquire the mutex.  May spin/sleep if contended.
void mutex_lock(mutex_t *m);

/// @brief Release the mutex.
void mutex_unlock(mutex_t *m);

/// @brief Try to acquire without blocking.
/// @return true if acquired.
bool mutex_trylock(mutex_t *m);
