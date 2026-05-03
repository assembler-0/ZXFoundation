// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/waitqueue.h
//
/// @brief Intrusive wait-queue for blocking synchronization primitives.
///
///        DESIGN
///        ======
///        A wait-queue is a spinlock-protected singly-linked list of
///        wq_entry_t nodes.  Each node is stack-allocated by the waiter
///        and embedded in the list for the duration of the wait.  This
///        avoids any dynamic allocation in the sleep path — critical for
///        a kernel that may need to sleep before the memory allocator is
///        ready.
///
///        WAKE PROTOCOL
///        =============
///        The waker sets entry->done = 1 (atomic store) then issues a
///        full barrier.  The sleeper polls entry->done in a tight loop
///        (or, once scheduling exists, yields the CPU).  The barrier on
///        the waker side ensures the sleeper sees all stores made before
///        the wake.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/atomic.h>
#include <zxfoundation/spinlock.h>

/// @brief One waiter node.  Stack-allocated by the sleeping thread.
typedef struct wq_entry {
    atomic_t         done;   ///< Set to 1 by the waker.
    struct wq_entry *next;   ///< Next entry in the queue (NULL = tail).
} wq_entry_t;

/// @brief Wait-queue head.
typedef struct {
    spinlock_t  lock;
    wq_entry_t *head;   ///< First waiter (NULL = empty).
} waitqueue_t;

#define WAITQUEUE_INIT  { .lock = SPINLOCK_INIT, .head = nullptr }
#define DEFINE_WAITQUEUE(name)  waitqueue_t name = WAITQUEUE_INIT

/// @brief Runtime initialize a wait-queue.
static inline void waitqueue_init(waitqueue_t *wq) {
    spin_lock_init(&wq->lock);
    wq->head = nullptr;
}

/// @brief Add entry to the tail of the queue and spin until woken.
///        Caller must zero-initialize entry->done before calling.
void waitqueue_wait(waitqueue_t *wq, wq_entry_t *entry);

/// @brief Wake the first waiter (FIFO).
/// @return true if a waiter was woken.
bool waitqueue_wake_one(waitqueue_t *wq);

/// @brief Wake all waiters.
void waitqueue_wake_all(waitqueue_t *wq);
