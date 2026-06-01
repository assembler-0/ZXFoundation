/// SPDX-License-Identifier: Apache-2.0
/// @file semaphore.h
/// @brief Counting semaphore.

#pragma once

#include <zxfoundation/types.h>
#include <arch/s390x/cpu/atomic.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sync/waitqueue.h>

/// @brief Counting semaphore structure.
///
/// Semaphores allow a fixed number of threads to access a resource.
/// If the count is zero, threads attempting to acquire the semaphore will sleep.
typedef struct {
    atomic_t    count;     ///< Current semaphore count.
    spinlock_t  wait_lock; ///< Protects the waiters list and count transitions.
    waitqueue_t waiters;   ///< List of threads waiting for the semaphore.
} semaphore_t;

/// @brief Static initializer for a semaphore.
/// @param n Initial count.
#define SEMAPHORE_INIT(n) { .count = ATOMIC_INIT(n), .wait_lock = SPINLOCK_INIT, \
                            .waiters = WAITQUEUE_INIT }

/// @brief Runtime initialize a semaphore.
/// @param[in,out] s The semaphore.
/// @param[in]     n Initial count.
static inline void semaphore_init(semaphore_t *s, int32_t n) {
    atomic_set(&s->count, n);
    spin_lock_init(&s->wait_lock);
    waitqueue_init(&s->waiters);
}

/// @brief Decrement (acquire) the semaphore.
/// @param[in,out] s The semaphore.
/// @note If the count is <= 0, the calling thread will block and sleep until
///       another thread calls semaphore_up().
/// @warning Must not be called from interrupt context.
void semaphore_down(semaphore_t *s);

/// @brief Increment (release) the semaphore.
/// @param[in,out] s The semaphore.
/// @note If there are waiters, one is woken.
void semaphore_up(semaphore_t *s);

/// @brief Try to decrement the semaphore without blocking.
/// @param[in,out] s The semaphore.
/// @return true if the semaphore was acquired, false if the count was 0.
bool semaphore_trydown(semaphore_t *s);
