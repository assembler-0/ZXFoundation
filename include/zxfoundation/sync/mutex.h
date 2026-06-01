/// SPDX-License-Identifier: Apache-2.0
/// @file mutex.h
/// @brief Sleeping mutex (non-recursive, non-IRQ-safe).

#pragma once

#include <arch/s390x/cpu/atomic.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sync/waitqueue.h>

/// @brief Sleeping mutex.
///
/// A mutex is a mutual exclusion primitive that allows a thread to sleep
/// while waiting for the lock.
///
/// @warning This mutex is NON-RECURSIVE. Attempting to acquire a mutex
///          already held by the calling thread will result in a deadlock.
/// @warning This mutex is NOT IRQ-SAFE. It must not be acquired from
///          hard-IRQ or soft-IRQ context as it may sleep.
typedef struct {
    atomic_t    state;   ///< 0 = unlocked, 1 = locked (no waiters), 2 = locked (waiters)
    spinlock_t  wait_lock; ///< Protects the waiters list.
    waitqueue_t waiters;   ///< List of threads waiting for the mutex.
} mutex_t;

/// @name Mutex State Values
/// @{
#define MUTEX_UNLOCKED  0   ///< Mutex is free.
#define MUTEX_LOCKED    1   ///< Mutex is held, but no other threads are waiting.
#define MUTEX_CONTESTED 2   ///< Mutex is held, and at least one thread is waiting.
/// @}

/// @name Initialization Macros
/// @{
/// @brief Static initializer for a mutex.
#define MUTEX_INIT  { .state = ATOMIC_INIT(0), .wait_lock = SPINLOCK_INIT, \
                      .waiters = WAITQUEUE_INIT }
/// @brief Define and initialize a static mutex.
#define DEFINE_MUTEX(name)  mutex_t name = MUTEX_INIT
/// @}

/// @brief Runtime initialize a mutex.
/// @param[in,out] m  The mutex to initialize.
static inline void mutex_init(mutex_t *m) {
    atomic_set(&m->state, 0);
    spin_lock_init(&m->wait_lock);
    waitqueue_init(&m->waiters);
}

/// @brief Acquire the mutex.
/// @param[in,out] m  The mutex to acquire.
/// @note If the mutex is already held, the calling thread will spin and
///       eventually sleep until it is woken by the current holder.
/// @warning Must not be called from interrupt context.
void mutex_lock(mutex_t *m);

/// @brief Release the mutex.
/// @param[in,out] m The mutex to release.
/// @note If there are waiters, the first waiter in the queue is woken.
void mutex_unlock(mutex_t *m);

/// @brief Try to acquire the mutex without blocking.
/// @param[in,out] m  The mutex to (try) acquire.
/// @return true if the mutex was acquired, false if it was already held.
bool mutex_trylock(mutex_t *m);
