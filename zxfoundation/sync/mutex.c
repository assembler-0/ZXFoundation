// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sync/mutex.c

#include <zxfoundation/sync/mutex.h>
#include <arch/s390x/cpu/processor.h>

// State values
#define MUTEX_UNLOCKED  0
#define MUTEX_LOCKED    1   // locked, no waiters
#define MUTEX_CONTESTED 2   // locked, waiters present

void mutex_lock(mutex_t *m) {
    // Fast path: CAS 0 → 1.
    if (atomic_cmpxchg(&m->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED)
        return;

    // Slow path: mark contested and enqueue.
    wq_entry_t entry;
    atomic_set(&entry.done, 0);
    entry.next = nullptr;

    irqflags_t flags;
    spin_lock_irqsave(&m->wait_lock, &flags);

    // Re-check under the wait_lock: the holder may have unlocked between
    // our failed CAS and acquiring wait_lock.
    if (atomic_cmpxchg(&m->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED) {
        spin_unlock_irqrestore(&m->wait_lock, flags);
        return;
    }

    // Mark contested so the unlocker knows to wake a waiter.
    atomic_set(&m->state, MUTEX_CONTESTED);

    // Enqueue at tail.
    if (!m->waiters.head) {
        m->waiters.head = &entry;
    } else {
        wq_entry_t *tail = m->waiters.head;
        while (tail->next)
            tail = tail->next;
        tail->next = &entry;
    }
    spin_unlock_irqrestore(&m->wait_lock, flags);

    // Sleep until the unlocker hands off ownership to us.
    while (!atomic_read(&entry.done))
        cpu_relax();

    barrier(); // acquire
}

void mutex_unlock(mutex_t *m) {
    // Release barrier: all stores inside the critical section must be
    // visible before we modify state or wake a waiter.
    smp_mb_release();

    irqflags_t flags;
    spin_lock_irqsave(&m->wait_lock, &flags);

    wq_entry_t *next = m->waiters.head;
    if (next) {
        m->waiters.head = next->next;
        // Keep state = MUTEX_LOCKED (or MUTEX_CONTESTED if more waiters).
        atomic_set(&m->state, m->waiters.head ? MUTEX_CONTESTED : MUTEX_LOCKED);
        spin_unlock_irqrestore(&m->wait_lock, flags);
        // Hand off: wake the next waiter directly.
        atomic_set(&next->done, 1);
    } else {
        atomic_set(&m->state, MUTEX_UNLOCKED);
        spin_unlock_irqrestore(&m->wait_lock, flags);
    }
}

bool mutex_trylock(mutex_t *m) {
    return atomic_cmpxchg(&m->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED;
}
