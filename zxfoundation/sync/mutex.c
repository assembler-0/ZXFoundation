// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sync/mutex.c

#include <zxfoundation/sync/mutex.h>
#include <arch/s390x/cpu/processor.h>

#define MUTEX_UNLOCKED  0
#define MUTEX_LOCKED    1   // locked, no waiters
#define MUTEX_CONTESTED 2   // locked, waiters present

void mutex_lock(mutex_t *m) {
    if (atomic_cmpxchg(&m->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED)
        return;

    wq_entry_t entry;
    atomic_set(&entry.done, 0);
    entry.next = nullptr;

    irqflags_t flags;
    spin_lock_irqsave(&m->wait_lock, &flags);

    if (atomic_cmpxchg(&m->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED) {
        spin_unlock_irqrestore(&m->wait_lock, flags);
        return;
    }

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

    while (!atomic_read(&entry.done))
        cpu_relax();

    barrier(); // acquire
}

void mutex_unlock(mutex_t *m) {
    smp_mb_release();

    irqflags_t flags;
    spin_lock_irqsave(&m->wait_lock, &flags);

    wq_entry_t *next = m->waiters.head;
    if (next) {
        m->waiters.head = next->next;
        atomic_set(&m->state, m->waiters.head ? MUTEX_CONTESTED : MUTEX_LOCKED);
        spin_unlock_irqrestore(&m->wait_lock, flags);
        atomic_set(&next->done, 1);
    } else {
        atomic_set(&m->state, MUTEX_UNLOCKED);
        spin_unlock_irqrestore(&m->wait_lock, flags);
    }
}

bool mutex_trylock(mutex_t *m) {
    return atomic_cmpxchg(&m->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED;
}
