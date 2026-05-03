// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sync/waitqueue.c

#include <zxfoundation/sync/waitqueue.h>
#include <arch/s390x/cpu/processor.h>

void waitqueue_wait(waitqueue_t *wq, wq_entry_t *entry) {
    atomic_set(&entry->done, 0);
    entry->next = nullptr;

    // Enqueue at the tail under the lock.
    irqflags_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    if (!wq->head) {
        wq->head = entry;
    } else {
        wq_entry_t *tail = wq->head;
        while (tail->next)
            tail = tail->next;
        tail->next = entry;
    }
    spin_unlock_irqrestore(&wq->lock, flags);

    // Spin until the waker sets done.
    while (!atomic_read(&entry->done))
        cpu_relax();

    // Acquire barrier: ensure we see all stores the waker made before
    // setting done.  On s390x TSO a compiler barrier suffices for the
    // acquire side (loads are not reordered with prior loads/stores).
    barrier();
}

bool waitqueue_wake_one(waitqueue_t *wq) {
    irqflags_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    wq_entry_t *entry = wq->head;
    if (entry)
        wq->head = entry->next;
    spin_unlock_irqrestore(&wq->lock, flags);

    if (!entry)
        return false;

    // Release barrier before setting done: all stores made by the waker
    // (e.g. writing the mutex owner field) must be visible to the waiter
    // before it sees done == 1.
    smp_mb_release();
    atomic_set(&entry->done, 1);
    return true;
}

void waitqueue_wake_all(waitqueue_t *wq) {
    // Drain the list under the lock, then wake each entry outside the lock
    // to avoid holding the lock while the woken thread runs.
    irqflags_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    wq_entry_t *list = wq->head;
    wq->head = nullptr;
    spin_unlock_irqrestore(&wq->lock, flags);

    while (list) {
        wq_entry_t *next = list->next;
        smp_mb_release();
        atomic_set(&list->done, 1);
        list = next;
    }
}
