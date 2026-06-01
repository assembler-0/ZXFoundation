/// SPDX-License-Identifier: Apache-2.0
/// @file rwlock.h
/// @brief Reader-writer spinlock.
/// @note This implementation is writer-biased (or at least gives writers
///       exclusive access as soon as they claim the lock word).
#pragma once

#include <zxfoundation/types.h>
#include <arch/s390x/cpu/atomic.h>

/// @brief Sentinel value indicating a writer holds the lock.
#define RW_WRITER   INT32_MIN

/// @brief Reader-writer spinlock structure.
typedef struct {
    atomic_t count;   ///< 0 = free, >0 = reader count, RW_WRITER = held by writer.
} rwlock_t;

/// @name Rwlock Initialization
/// @{
/// @brief Static initializer for an rwlock.
#define RWLOCK_INIT         { .count = ATOMIC_INIT(0) }
/// @brief Define and initialize a static rwlock.
#define DEFINE_RWLOCK(name) rwlock_t name = RWLOCK_INIT

/// @brief Runtime initialize an rwlock.
/// @param[in,out] rw The rwlock to initialize.
static inline void rwlock_init(rwlock_t *rw) {
    atomic_set(&rw->count, 0);
}
/// @}

/// @brief Acquire the lock for reading.
/// @param[in,out] rw The rwlock.
/// @note Spins until no writer holds the lock. Multiple readers can hold the
///       lock simultaneously.
void read_lock(rwlock_t *rw);

/// @brief Release a read lock.
/// @param[in,out] rw The rwlock.
void read_unlock(rwlock_t *rw);

/// @brief Acquire the lock for writing.
/// @param[in,out] rw The rwlock.
/// @note Spins until all current readers and any existing writer have released
///       the lock. Provides exclusive access.
void write_lock(rwlock_t *rw);

/// @brief Release a write lock.
/// @param[in,out] rw The rwlock.
void write_unlock(rwlock_t *rw);

/// @brief Try to acquire the lock for reading without blocking.
/// @param[in,out] rw The rwlock.
/// @return true if acquired, false if a writer holds the lock.
bool read_trylock(rwlock_t *rw);

/// @brief Try to acquire the lock for writing without blocking.
/// @param[in,out] rw The rwlock.
/// @return true if acquired, false if any readers or a writer holds the lock.
bool write_trylock(rwlock_t *rw);
