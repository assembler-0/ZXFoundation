// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/spinlock.h
//
/// @brief Ticket spinlock for SMP-safe kernel locking.
///
///        DESIGN: Ticket lock (Mellor-Crummey & Scott, 1991).
///        A ticket lock gives strict FIFO ordering — CPUs acquire the lock
///        in the order they requested it.  This prevents starvation and
///        gives predictable worst-case latency, which matters for a kernel
///        with hard real-time I/O paths.
///
///        The lock word is a pair of 16-bit counters packed into a 32-bit
///        atomic: high half = next ticket (handed out on lock), low half =
///        now-serving (incremented on unlock).  A CPU holds the lock when
///        its ticket == now_serving.
///
///        IRQSAVE VARIANTS
///        ================
///        spin_lock_irqsave / spin_unlock_irqrestore disable/restore the
///        local CPU's interrupt mask around the critical section.  This is
///        mandatory when a lock is shared between process context and an
///        interrupt handler — without it, the interrupt handler can attempt
///        to acquire a lock already held by the interrupted code on the same
///        CPU, causing a deadlock.
///
///        On s390x, interrupts are controlled via the PSW mask bits.
///        We use SSM/STOSM to atomically read-and-modify the mask.

#pragma once

#include <arch/s390x/cpu/irq.h>
#include <zxfoundation/atomic.h>

// ---------------------------------------------------------------------------
// spinlock_t
// ---------------------------------------------------------------------------

typedef struct {
    /// Ticket counter: bits[31:16] = next, bits[15:0] = now_serving.
    /// Packed into one 32-bit word so the unlock (increment now_serving)
    /// is a single CS instruction — no separate atomic needed.
    atomic_t tickets;
} spinlock_t;

#define SPINLOCK_INIT   { .tickets = ATOMIC_INIT(0) }

/// @brief Statically initialise a spinlock.
#define DEFINE_SPINLOCK(name)   spinlock_t name = SPINLOCK_INIT

/// @brief Runtime initialise a spinlock (for dynamically allocated locks).
static inline void spin_lock_init(spinlock_t *lock) {
    atomic_set(&lock->tickets, 0);
}

/// @brief Acquire the spinlock.  Spins until the lock is granted.
///        Issues a full barrier on acquire to ensure all subsequent
///        memory accesses see stores from the previous lock holder.
void spin_lock(spinlock_t *lock);

/// @brief Release the spinlock.
///        Issues a release barrier before incrementing now_serving so
///        all stores inside the critical section are visible to the next
///        lock holder before it enters.
void spin_unlock(spinlock_t *lock);

/// @brief Try to acquire the lock without spinning.
/// @return true if the lock was acquired, false if it was already held.
bool spin_trylock(spinlock_t *lock);

/// @brief Acquire the spinlock and disable local interrupts.
/// @param lock   The spinlock to acquire.
/// @param flags  Output: saved IRQ flags (pass to spin_unlock_irqrestore).
static inline void spin_lock_irqsave(spinlock_t *lock, irqflags_t *flags) {
    *flags = arch_local_save_flags();
    arch_local_irq_disable();
    spin_lock(lock);
}

/// @brief Release the spinlock and restore local interrupt state.
/// @param lock   The spinlock to release.
/// @param flags  Saved IRQ flags from spin_lock_irqsave.
static inline void spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags) {
    spin_unlock(lock);
    arch_local_irq_restore(flags);
}
