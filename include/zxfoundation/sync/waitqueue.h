/// SPDX-License-Identifier: Apache-2.0
/// @file waitqueue.h
/// @brief Intrusive wait-queue for blocking synchronization primitives.

#pragma once

#include <arch/s390x/cpu/atomic.h>
#include <zxfoundation/sync/spinlock.h>

/// @brief One waiter node.
///
/// Nodes are typically stack-allocated by the sleeping thread and embedded
/// into the wait queue for the duration of the wait.
typedef struct wq_entry {
    atomic_t         done;   ///< Set to 1 by the waker to signal completion.
    struct wq_entry *next;   ///< Linkage to the next entry in the queue.
} wq_entry_t;

/// @brief Wait-queue head.
typedef struct {
    spinlock_t  lock;       ///< Protects the list of waiters.
    wq_entry_t *head;       ///< First waiter in the queue (FIFO).
} waitqueue_t;

/// @name Wait-queue Initialization
/// @{
/// @brief Static initializer for a wait-queue.
#define WAITQUEUE_INIT  { .lock = SPINLOCK_INIT, .head = nullptr }
/// @brief Define and initialize a static wait-queue.
#define DEFINE_WAITQUEUE(name)  waitqueue_t name = WAITQUEUE_INIT

/// @brief Runtime initialize a wait-queue.
/// @param[in,out] wq The wait-queue.
static inline void waitqueue_init(waitqueue_t *wq) {
    spin_lock_init(&wq->lock);
    wq->head = nullptr;
}
/// @}

/// @brief Add an entry to the wait-queue and block until woken.
/// @param[in,out] wq    The wait-queue.
/// @param[in,out] entry The waiter node (must be zero-initialized).
/// @note The calling thread will spin until entry->done becomes 1.
void waitqueue_wait(waitqueue_t *wq, wq_entry_t *entry);

/// @brief Wake the first waiter in the queue.
/// @param[in,out] wq The wait-queue.
/// @return true if a waiter was woken, false if the queue was empty.
bool waitqueue_wake_one(waitqueue_t *wq);

/// @brief Wake all waiters in the queue.
/// @param[in,out] wq The wait-queue.
void waitqueue_wake_all(waitqueue_t *wq);
