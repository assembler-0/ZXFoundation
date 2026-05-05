// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/rwlock.h
//
/// @brief Reader-writer spinlock.
///
#pragma once

#include <zxfoundation/types.h>
#include <arch/s390x/cpu/atomic.h>

#define RW_WRITER   INT32_MIN   ///< Sentinel: writer holds the lock.

typedef struct {
    atomic_t count;   ///< 0=free, >0=readers, RW_WRITER=writer
} rwlock_t;

#define RWLOCK_INIT         { .count = ATOMIC_INIT(0) }
#define DEFINE_RWLOCK(name) rwlock_t name = RWLOCK_INIT

static inline void rwlock_init(rwlock_t *rw) {
    atomic_set(&rw->count, 0);
}

/// @brief Acquire for reading.  Spins until no writer holds the lock.
void read_lock(rwlock_t *rw);

/// @brief Release a read lock.
void read_unlock(rwlock_t *rw);

/// @brief Acquire for writing.  Spins until all readers and writers drain.
void write_lock(rwlock_t *rw);

/// @brief Release a write lock.
void write_unlock(rwlock_t *rw);

/// @brief Non-blocking read-lock attempt.
bool read_trylock(rwlock_t *rw);

/// @brief Non-blocking write-lock attempt.
bool write_trylock(rwlock_t *rw);
