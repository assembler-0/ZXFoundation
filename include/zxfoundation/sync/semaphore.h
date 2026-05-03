// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/semaphore.h
//
/// @brief Counting semaphore.
///
///        DESIGN
///        ======
///        count is a signed atomic.  down() decrements; if the result is
///        negative the caller enqueues on the wait-queue and sleeps.
///        up() increments; if the pre-increment value was negative, a
///        waiter exists and is woken.
///
///        The wait_lock spinlock serializes the transition between the
///        atomic count update and the wait-queue manipulation.  Without
///        it, a waker could call waitqueue_wake_one() between the moment
///        a sleeper decrements count and the moment it enqueues itself,
///        losing the wake.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/atomic.h>
#include <zxfoundation/spinlock.h>
#include <zxfoundation/sync/waitqueue.h>

typedef struct {
    atomic_t    count;
    spinlock_t  wait_lock;
    waitqueue_t waiters;
} semaphore_t;

#define SEMAPHORE_INIT(n) { .count = ATOMIC_INIT(n), .wait_lock = SPINLOCK_INIT, \
                            .waiters = WAITQUEUE_INIT }

/// @brief Runtime initialize a semaphore with initial count n.
static inline void semaphore_init(semaphore_t *s, int32_t n) {
    atomic_set(&s->count, n);
    spin_lock_init(&s->wait_lock);
    waitqueue_init(&s->waiters);
}

/// @brief Decrement (acquire).  Blocks if count would go negative.
void semaphore_down(semaphore_t *s);

/// @brief Increment (release).  Wakes one waiter if any are sleeping.
void semaphore_up(semaphore_t *s);

/// @brief Non-blocking decrement.
/// @return true if acquired (count was > 0).
bool semaphore_trydown(semaphore_t *s);
