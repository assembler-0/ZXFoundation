// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/seqlock.h
//
/// @brief Sequence Locks — reader-writer lock with lockless readers.

#pragma once

#include <zxfoundation/common.h>
#include <arch/s390x/cpu/atomic.h>
#include <arch/s390x/cpu/irq.h>
#include <zxfoundation/sync/spinlock.h>

// ---------------------------------------------------------------------------
// seqcount_t
// ---------------------------------------------------------------------------

typedef struct {
    unsigned sequence;
} seqcount_t;

#define SEQCOUNT_INIT { .sequence = 0 }

static inline void seqcount_init(seqcount_t *s) {
    s->sequence = 0;
}

/// @brief Begin reading a sequence.
/// @return The sequence number to be checked later.
static inline unsigned read_seqcount_begin(const seqcount_t *s) {
    unsigned seq;
    do {
        seq = read_once(s->sequence);
        smp_mb_acquire();
    } while (seq & 1);
    return seq;
}

/// @brief Check if the sequence changed since read_seqcount_begin().
/// @return true if a retry is needed, false otherwise.
static inline bool read_seqcount_retry(const seqcount_t *s, unsigned start) {
    // Acquire barrier: ensure data loads happen before reading the sequence again.
    smp_mb_acquire();
    return read_once(s->sequence) != start;
}

/// @brief Begin a sequence write. The caller MUST hold a writer lock!
static inline void write_seqcount_begin(seqcount_t *s) {
    write_once(s->sequence, s->sequence + 1);
    // Release barrier: ensure sequence increment is visible before data stores.
    smp_mb_release();
}

/// @brief End a sequence write. The caller MUST hold a writer lock!
static inline void write_seqcount_end(seqcount_t *s) {
    // Release barrier: ensure data stores are visible before sequence increment.
    smp_mb_release();
    write_once(s->sequence, s->sequence + 1);
}

// ---------------------------------------------------------------------------
// seqlock_t
// ---------------------------------------------------------------------------

typedef struct {
    seqcount_t seqcount;
    spinlock_t lock;
} seqlock_t;

#define SEQLOCK_INIT { .seqcount = SEQCOUNT_INIT, .lock = SPINLOCK_INIT }

static inline void seqlock_init(seqlock_t *sl) {
    seqcount_init(&sl->seqcount);
    spin_lock_init(&sl->lock);
}

static inline unsigned read_seqbegin(const seqlock_t *sl) {
    return read_seqcount_begin(&sl->seqcount);
}

static inline bool read_seqretry(const seqlock_t *sl, unsigned start) {
    return read_seqcount_retry(&sl->seqcount, start);
}

/// @brief Acquire the seqlock for writing. Disables IRQs to prevent deadlocks
///        if an IRQ handler tries to read the same seqlock.
static inline void write_seqlock_irqsave(seqlock_t *sl, irqflags_t *flags) {
    spin_lock_irqsave(&sl->lock, flags);
    write_seqcount_begin(&sl->seqcount);
}

/// @brief Release the seqlock after writing. Restores IRQs.
static inline void write_sequnlock_irqrestore(seqlock_t *sl, irqflags_t flags) {
    write_seqcount_end(&sl->seqcount);
    spin_unlock_irqrestore(&sl->lock, flags);
}

/// @brief Acquire the seqlock for writing without IRQ protection.
///        DANGEROUS: Only use if you are certain this seqlock is never read
///        from an IRQ handler on the same CPU.
static inline void write_seqlock(seqlock_t *sl) {
    spin_lock(&sl->lock);
    write_seqcount_begin(&sl->seqcount);
}

static inline void write_sequnlock(seqlock_t *sl) {
    write_seqcount_end(&sl->seqcount);
    spin_unlock(&sl->lock);
}
