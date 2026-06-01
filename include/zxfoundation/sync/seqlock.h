/// SPDX-License-Identifier: Apache-2.0
/// @file seqlock.h
/// @brief Sequence Locks — reader-writer lock with lockless readers.

#pragma once

#include <zxfoundation/common.h>
#include <arch/s390x/cpu/atomic.h>
#include <arch/s390x/cpu/irq.h>
#include <zxfoundation/sync/spinlock.h>

/// @brief Sequence counter.
typedef struct {
    unsigned sequence; ///< 0 = even (stable), odd = writer is active.
} seqcount_t;

/// @brief Static initializer for a seqcount.
#define SEQCOUNT_INIT { .sequence = 0 }

/// @brief Runtime initialize a seqcount.
/// @param[in,out] s The seqcount.
static inline void seqcount_init(seqcount_t *s) {
    s->sequence = 0;
}

/// @brief Begin reading a sequence.
/// @param[in] s The seqcount.
/// @return The sequence number to be passed to read_seqcount_retry().
/// @note If the returned value is odd, the reader will spin until it
///       becomes even (writer finished).
static inline unsigned read_seqcount_begin(const seqcount_t *s) {
    unsigned seq;
    do {
        seq = read_once(s->sequence);
        smp_mb_acquire();
    } while (seq & 1);
    return seq;
}

/// @brief Check if the sequence changed since read_seqcount_begin().
/// @param[in] s     The seqcount.
/// @param[in] start The sequence number returned by read_seqcount_begin().
/// @return true if a retry is needed, false if the read data is consistent.
static inline bool read_seqcount_retry(const seqcount_t *s, unsigned start) {
    smp_mb_acquire();
    return read_once(s->sequence) != start;
}

/// @brief Begin a sequence write.
/// @param[in,out] s The seqcount.
/// @warning The caller MUST hold the associated writer lock!
static inline void write_seqcount_begin(seqcount_t *s) {
    write_once(s->sequence, s->sequence + 1);
    smp_mb_release();
}

/// @brief End a sequence write.
/// @param[in,out] s The seqcount.
/// @warning The caller MUST hold the associated writer lock!
static inline void write_seqcount_end(seqcount_t *s) {
    smp_mb_release();
    write_once(s->sequence, s->sequence + 1);
}

/// @brief Sequence lock structure.
///
/// Combines a seqcount with a spinlock for writers.
typedef struct {
    seqcount_t seqcount; ///< The sequence counter.
    spinlock_t lock;     ///< Spinlock protecting writers.
} seqlock_t;

/// @brief Static initializer for a seqlock.
#define SEQLOCK_INIT { .seqcount = SEQCOUNT_INIT, .lock = SPINLOCK_INIT }

/// @brief Runtime initialize a seqlock.
/// @param[in,out] sl The seqlock.
static inline void seqlock_init(seqlock_t *sl) {
    seqcount_init(&sl->seqcount);
    spin_lock_init(&sl->lock);
}

/// @brief Begin a seqlock read-side critical section.
/// @param[in] sl The seqlock.
/// @return Current sequence number.
static inline unsigned read_seqbegin(const seqlock_t *sl) {
    return read_seqcount_begin(&sl->seqcount);
}

/// @brief End a seqlock read-side critical section and check for consistency.
/// @param[in] sl    The seqlock.
/// @param[in] start Sequence number from read_seqbegin().
/// @return true if retry is needed, false if read was successful.
static inline bool read_seqretry(const seqlock_t *sl, unsigned start) {
    return read_seqcount_retry(&sl->seqcount, start);
}

/// @brief Acquire the seqlock for writing, disabling IRQs.
/// @param[in,out] sl    The seqlock.
/// @param[out]    flags Pointer to store interrupt flags.
/// @note Disables IRQs to prevent deadlocks if an IRQ handler tries to
///       read the same seqlock.
static inline void write_seqlock_irqsave(seqlock_t *sl, irqflags_t *flags) {
    spin_lock_irqsave(&sl->lock, flags);
    write_seqcount_begin(&sl->seqcount);
}

/// @brief Release the seqlock after writing, restoring IRQs.
/// @param[in,out] sl    The seqlock.
/// @param[in]     flags Interrupt flags to restore.
static inline void write_sequnlock_irqrestore(seqlock_t *sl, irqflags_t flags) {
    write_seqcount_end(&sl->seqcount);
    spin_unlock_irqrestore(&sl->lock, flags);
}

/// @brief Acquire the seqlock for writing without IRQ protection.
/// @param[in,out] sl The seqlock.
/// @warning Use only if this seqlock is NEVER read from IRQ context.
static inline void write_seqlock(seqlock_t *sl) {
    spin_lock(&sl->lock);
    write_seqcount_begin(&sl->seqcount);
}

/// @brief Release the seqlock after writing without IRQ protection.
/// @param[in,out] sl The seqlock.
static inline void write_sequnlock(seqlock_t *sl) {
    write_seqcount_end(&sl->seqcount);
    spin_unlock(&sl->lock);
}
