// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/spinlock.h
//
/// @brief MCS-based queue spinlock — cache-friendly under contention.
///
///        The 32-bit lock word is partitioned as:
///          bits [31:16]  tail    — encoded (cpu+1, depth) of the queue tail
///          bits [15: 8]  pending — set by the second waiter
///          bits  [7: 0]  locked  — 1 when the lock is held
///
///        Implemented in zxfoundation/sync/spinlock.c.

#pragma once

#include <arch/s390x/cpu/atomic.h>
#include <arch/s390x/cpu/irq.h>

// ---------------------------------------------------------------------------
// Lock word layout
// ---------------------------------------------------------------------------

#define _Q_LOCKED_OFFSET    0
#define _Q_LOCKED_BITS      8
#define _Q_LOCKED_MASK      0x000000FFU
#define _Q_LOCKED_VAL       1U

#define _Q_PENDING_OFFSET   8
#define _Q_PENDING_BITS     8
#define _Q_PENDING_MASK     0x0000FF00U
#define _Q_PENDING_VAL      (1U << _Q_PENDING_OFFSET)

#define _Q_TAIL_OFFSET      16
#define _Q_TAIL_BITS        16
#define _Q_TAIL_MASK        0xFFFF0000U

/// Maximum lock nesting depth per CPU (NMI + IRQ + process = 3 is typical).
#define MAX_LOCK_DEPTH      4U

// ---------------------------------------------------------------------------
// MCS queue node
// ---------------------------------------------------------------------------

/// @brief One MCS queue node.  Lives in the per-CPU area; never heap-allocated.
typedef struct mcs_node {
    struct mcs_node *next;  ///< Next waiter in the queue (nullptr = tail).
    atomic_t         locked; ///< 1 when this node's turn has arrived.
} mcs_node_t;

// ---------------------------------------------------------------------------
// qspinlock_t
// ---------------------------------------------------------------------------

typedef struct {
    atomic_t val;   ///< Packed lock word: [tail|pending|locked].
} spinlock_t;

#define SPINLOCK_INIT  { .val = ATOMIC_INIT(0) }
#define DEFINE_SPINLOCK(name)  spinlock_t name = SPINLOCK_INIT

static inline void spin_lock_init(spinlock_t *lock) {
    atomic_set(&lock->val, 0);
}

static inline bool spin_is_locked(const spinlock_t *lock) {
    return (uint32_t)atomic_read((atomic_t *)&lock->val) & _Q_LOCKED_MASK;
}

// ---------------------------------------------------------------------------
// Public API (implemented in arch/s390x/cpu/qspinlock.c)
// ---------------------------------------------------------------------------

/// @brief Acquire the qspinlock.  Fast path: single CAS when uncontended.
///        Slow path: enqueue an MCS node and spin on it.
void spin_lock(spinlock_t *lock);

/// @brief Release the qspinlock.
void spin_unlock(spinlock_t *lock);

/// @brief Non-blocking acquire.
/// @return true if acquired, false if already held.
bool spin_trylock(spinlock_t *lock);

static inline void spin_lock_irqsave(spinlock_t *lock, irqflags_t *flags) {
    *flags = arch_local_save_flags();
    arch_local_irq_disable();
    spin_lock(lock);
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags) {
    spin_unlock(lock);
    arch_local_irq_restore(flags);
}
