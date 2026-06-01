/// SPDX-License-Identifier: Apache-2.0
/// @file semaphore.c
/// @brief Counting semaphore implementation.

#include <zxfoundation/sync/semaphore.h>
#include <arch/s390x/cpu/processor.h>

/// @brief Decrement (acquire) the semaphore.
/// @param[in,out] s The semaphore.
void semaphore_down(semaphore_t *s) {
    int32_t cur;
    // Fast path: try to grab a unit if available.
    do {
        cur = atomic_read(&s->count);
        if (cur <= 0)
            goto slow;
    } while (atomic_cmpxchg(&s->count, cur, cur - 1) != cur);
    barrier();
    return;

slow:;
    wq_entry_t entry;
    atomic_set(&entry.done, 0);
    entry.next = nullptr;

    irqflags_t flags;
    spin_lock_irqsave(&s->wait_lock, &flags);

    // Re-check under the lock to avoid missing a wake.
    cur = atomic_read(&s->count);
    if (cur > 0) {
        atomic_add_return(&s->count, -1);
        spin_unlock_irqrestore(&s->wait_lock, flags);
        barrier();
        return;
    }

    // Record our presence by taking the count negative.
    atomic_add_return(&s->count, -1);

    // Enqueue at tail (FIFO).
    if (!s->waiters.head) {
        s->waiters.head = &entry;
    } else {
        wq_entry_t *tail = s->waiters.head;
        while (tail->next)
            tail = tail->next;
        tail->next = &entry;
    }
    spin_unlock_irqrestore(&s->wait_lock, flags);

    // Spin until woken by semaphore_up().
    while (!atomic_read(&entry.done))
        arch_cpu_relax();

    barrier();
}

/// @brief Increment (release) the semaphore.
/// @param[in,out] s The semaphore.
void semaphore_up(semaphore_t *s) {
    smp_mb_release();

    irqflags_t flags;
    spin_lock_irqsave(&s->wait_lock, &flags);

    // Increment count. atomic_add_return returns the NEW value.
    int32_t old = atomic_add_return(&s->count, 1) - 1;
    wq_entry_t *entry = nullptr;
    if (old < 0) {
        // A waiter is sleeping; pop it.
        entry = s->waiters.head;
        if (entry)
            s->waiters.head = entry->next;
    }
    spin_unlock_irqrestore(&s->wait_lock, flags);

    // Signal the waiter.
    if (entry)
        atomic_set(&entry->done, 1);
}

/// @brief Try to decrement the semaphore without blocking.
/// @param[in,out] s The semaphore.
/// @return true if acquired, false if count was <= 0.
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
