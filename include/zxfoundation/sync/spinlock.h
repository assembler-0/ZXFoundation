/// SPDX-License-Identifier: Apache-2.0
/// @file spinlock.h
/// @brief MCS-based queue spinlock — cache-friendly under contention.
#pragma once

#include <arch/s390x/cpu/atomic.h>
#include <arch/s390x/cpu/irq.h>

/// @name Lock Word Layout Constants
/// @{
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
/// @}

/// @brief Maximum lock nesting depth per CPU.
/// @note Typically NMI + IRQ + softirq + process = 4.
#define MAX_LOCK_DEPTH      4U

/// @brief One MCS queue node.
/// @note These nodes are allocated in per-CPU memory; never on the heap.
typedef struct mcs_node {
    struct mcs_node *next;   ///< Next waiter in the queue (nullptr = tail).
    atomic_t         locked; ///< 1 when this node's turn has arrived.
} mcs_node_t;

/// @brief Queue spinlock structure.
typedef struct {
    atomic_t val;   ///< Packed lock word: [tail|pending|locked].
} spinlock_t;

/// @name Spinlock Initialization
/// @{
/// @brief Static initializer for a spinlock.
#define SPINLOCK_INIT  { .val = ATOMIC_INIT(0) }
/// @brief Define and initialize a static spinlock.
#define DEFINE_SPINLOCK(name)  spinlock_t name = SPINLOCK_INIT

/// @brief Runtime initialize a spinlock.
/// @param[in,out] lock The spinlock to initialize.
static inline void spin_lock_init(spinlock_t *lock) {
    atomic_set(&lock->val, 0);
}
/// @}

/// @brief Check if the spinlock is currently held.
/// @param[in] lock The spinlock.
/// @return true if held, false otherwise.
static inline bool spin_is_locked(const spinlock_t *lock) {
    return (uint32_t)atomic_read((atomic_t *)&lock->val) & _Q_LOCKED_MASK;
}

/// @brief Acquire the spinlock.
/// @param[in,out] lock The spinlock to acquire.
/// @note Fast path: single CAS when uncontended.
///       Slow path: enqueues an MCS node and spins on it.
/// @warning Does not disable interrupts. Use only if the lock is not
///          acquired in interrupt context, or if interrupts are already disabled.
void spin_lock(spinlock_t *lock);

/// @brief Release the spinlock.
/// @param[in,out] lock The spinlock to release.
void spin_unlock(spinlock_t *lock);

/// @brief Try to acquire the spinlock without blocking.
/// @param[in,out] lock The spinlock to (try) acquire.
/// @return true if the lock was acquired, false if it was already held.
bool spin_trylock(spinlock_t *lock);

/// @brief Acquire the spinlock while saving interrupt state and disabling IRQs.
/// @param[in,out] lock  The spinlock.
/// @param[out]    flags Pointer to store the current interrupt flags.
/// @note This is the safest way to acquire a spinlock that is shared with
///       interrupt handlers.
static inline void spin_lock_irqsave(spinlock_t *lock, irqflags_t *flags) {
    *flags = arch_local_save_flags();
    arch_local_irq_disable();
    spin_lock(lock);
}

/// @brief Release the spinlock and restore interrupt state.
/// @param[in,out] lock  The spinlock.
/// @param[in]     flags Interrupt flags to restore.
static inline void spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags) {
    spin_unlock(lock);
    arch_local_irq_restore(flags);
}
