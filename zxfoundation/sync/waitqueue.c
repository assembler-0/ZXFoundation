/// SPDX-License-Identifier: Apache-2.0
/// @file waitqueue.c
/// @brief Intrusive wait-queue implementation.

#include <zxfoundation/sync/waitqueue.h>
#include <arch/s390x/cpu/processor.h>

/// @brief Add entry to the queue and block until woken.
/// @param[in,out] wq    The wait-queue.
/// @param[in,out] entry The waiter node.
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

    // Wait loop.
    // TODO: replace with scheduler-aware blocking.
    while (!atomic_read(&entry->done))
        arch_cpu_relax();

    // Ensure all stores by the waker are visible.
    barrier();
}

/// @brief Wake the first waiter in the queue.
/// @param[in,out] wq The wait-queue.
/// @return true if a waiter was woken, false if the queue was empty.
bool waitqueue_wake_one(waitqueue_t *wq) {
    irqflags_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    wq_entry_t *entry = wq->head;
    if (entry)
        wq->head = entry->next;
    spin_unlock_irqrestore(&wq->lock, flags);

    if (!entry)
        return false;

    // Release barrier to ensure all prior stores are visible to the sleeper.
    smp_mb_release();
    atomic_set(&entry->done, 1);
    return true;
}

/// @brief Wake all waiters in the queue.
/// @param[in,out] wq The wait-queue.
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
