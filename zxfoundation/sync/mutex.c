/// SPDX-License-Identifier: Apache-2.0
/// @file mutex.c
/// @brief Mutex implementation.

#include <zxfoundation/sync/mutex.h>
#include <arch/s390x/cpu/processor.h>

/// @brief Acquire the mutex.
/// @param[in,out] m  The mutex to acquire.
void mutex_lock(mutex_t *m) {
    // Fast path: try to grab the lock if it's currently free.
    if (atomic_cmpxchg(&m->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED)
        return;

    // Slow path: prepare to wait.
    wq_entry_t entry;
    atomic_set(&entry.done, 0);
    entry.next = nullptr;

    irqflags_t flags;
    spin_lock_irqsave(&m->wait_lock, &flags);

    // Re-check state now that we hold the wait_lock.
    if (atomic_cmpxchg(&m->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED) {
        spin_unlock_irqrestore(&m->wait_lock, flags);
        return;
    }

    // Mark as contested and enqueue.
    atomic_set(&m->state, MUTEX_CONTESTED);

    if (!m->waiters.head) {
        m->waiters.head = &entry;
    } else {
        wq_entry_t *tail = m->waiters.head;
        while (tail->next)
            tail = tail->next;
        tail->next = &entry;
    }
    spin_unlock_irqrestore(&m->wait_lock, flags);

    // Spin until woken.
    // TODO: replace with thread_block() when scheduler is available.
    while (!atomic_read(&entry.done))
        arch_cpu_relax();

    // Ensure all stores by the waker are visible.
    barrier();
}

/// @brief Release the mutex.
/// @param[in,out] m The mutex to release.
void mutex_unlock(mutex_t *m) {
    // Release barrier.
    smp_mb_release();

    irqflags_t flags;
    spin_lock_irqsave(&m->wait_lock, &flags);

    wq_entry_t *next = m->waiters.head;
    if (next) {
        m->waiters.head = next->next;
        // If there are more waiters, keep it CONTESTED.
        atomic_set(&m->state, m->waiters.head ? MUTEX_CONTESTED : MUTEX_LOCKED);
        spin_unlock_irqrestore(&m->wait_lock, flags);
        // Wake the waiter.
        atomic_set(&next->done, 1);
    } else {
        // No waiters, just unlock.
        atomic_set(&m->state, MUTEX_UNLOCKED);
        spin_unlock_irqrestore(&m->wait_lock, flags);
    }
}

/// @brief Try to acquire the mutex without blocking.
/// @param[in,out] m  The mutex to (try) acquire.
/// @return true if acquired, false if it was already held.
bool mutex_trylock(mutex_t *m) {
    return atomic_cmpxchg(&m->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED;
}
