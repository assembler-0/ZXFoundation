// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sync/semaphore.c

#include <zxfoundation/sync/semaphore.h>
#include <arch/s390x/cpu/processor.h>

void semaphore_down(semaphore_t *s) {
    int32_t cur;
    do {
        cur = atomic_read(&s->count);
        if (cur <= 0)
            goto slow;
    } while (atomic_cmpxchg(&s->count, cur, cur - 1) != cur);
    barrier(); // acquire
    return;

slow:;
    wq_entry_t entry;
    atomic_set(&entry.done, 0);
    entry.next = nullptr;

    irqflags_t flags;
    spin_lock_irqsave(&s->wait_lock, &flags);

    // Re-check under the lock.
    cur = atomic_read(&s->count);
    if (cur > 0) {
        // Someone called up() between our check and acquiring the lock.
        atomic_add_return(&s->count, -1);
        spin_unlock_irqrestore(&s->wait_lock, flags);
        barrier();
        return;
    }

    // Decrement into negative territory to record that we are waiting.
    atomic_add_return(&s->count, -1);

    // Enqueue at tail.
    if (!s->waiters.head) {
        s->waiters.head = &entry;
    } else {
        wq_entry_t *tail = s->waiters.head;
        while (tail->next)
            tail = tail->next;
        tail->next = &entry;
    }
    spin_unlock_irqrestore(&s->wait_lock, flags);

    while (!atomic_read(&entry.done))
        arch_cpu_relax();

    barrier(); // acquire
}

void semaphore_up(semaphore_t *s) {
    smp_mb_release();

    irqflags_t flags;
    spin_lock_irqsave(&s->wait_lock, &flags);

    int32_t old = atomic_add_return(&s->count, 1) - 1;
    wq_entry_t *entry = nullptr;
    if (old < 0) {
        // A waiter is sleeping; pop it.
        entry = s->waiters.head;
        if (entry)
            s->waiters.head = entry->next;
    }
    spin_unlock_irqrestore(&s->wait_lock, flags);

    if (entry)
        atomic_set(&entry->done, 1);
}

bool semaphore_trydown(semaphore_t *s) {
    int32_t cur;
    do {
        cur = atomic_read(&s->count);
        if (cur <= 0)
            return false;
    } while (atomic_cmpxchg(&s->count, cur, cur - 1) != cur);
    barrier();
    return true;
}
