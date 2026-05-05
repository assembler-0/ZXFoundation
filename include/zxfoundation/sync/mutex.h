// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/mutex.h
//
/// @brief Sleeping mutex (non-recursive, non-IRQ-safe).

#pragma once

#include <zxfoundation/types.h>
#include <arch/s390x/cpu/atomic.h>
#include <zxfoundation/sync/spinlock.h>
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
