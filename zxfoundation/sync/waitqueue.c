// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sync/waitqueue.c

#include <zxfoundation/sync/waitqueue.h>
#include <arch/s390x/cpu/processor.h>

void waitqueue_wait(waitqueue_t *wq, wq_entry_t *entry) {
    atomic_set(&entry->done, 0);
    entry->next = nullptr;

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

    while (!atomic_read(&entry->done))
        cpu_relax();

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

    smp_mb_release();
    atomic_set(&entry->done, 1);
    return true;
}

void waitqueue_wake_all(waitqueue_t *wq) {
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
