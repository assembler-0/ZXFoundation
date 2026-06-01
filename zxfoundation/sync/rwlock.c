/// SPDX-License-Identifier: Apache-2.0
/// @file rwlock.c
/// @brief Reader-writer spinlock implementation.

#include <zxfoundation/sync/rwlock.h>
#include <arch/s390x/cpu/processor.h>

/// @brief Acquire the lock for reading.
/// @param[in,out] rw The rwlock.
void read_lock(rwlock_t *rw) {
    int32_t cur;
    do {
        do {
            cur = atomic_read(&rw->count);
        } while (cur == RW_WRITER && (arch_cpu_relax(), true));
        // CAS: increment reader count only if still not RW_WRITER.
    } while (atomic_cmpxchg(&rw->count, cur, cur + 1) != cur);
    // Ensure data loads don't migrate before the lock is acquired.
    barrier();
}

/// @brief Release a read lock.
/// @param[in,out] rw The rwlock.
void read_unlock(rwlock_t *rw) {
    // Release barrier.
    smp_mb_release();
    atomic_add_return(&rw->count, -1);
}

/// @brief Acquire the lock for writing.
/// @param[in,out] rw The rwlock.
void write_lock(rwlock_t *rw) {
    while (atomic_cmpxchg(&rw->count, 0, RW_WRITER) != 0)
        arch_cpu_relax();
    // Ensure stores don't migrate before the lock is acquired.
    barrier();
}

/// @brief Release a write lock.
/// @param[in,out] rw The rwlock.
void write_unlock(rwlock_t *rw) {
    // Release barrier.
    smp_mb_release();
    atomic_set(&rw->count, 0);
}

/// @brief Non-blocking read-lock attempt.
/// @param[in,out] rw The rwlock.
/// @return true if acquired, false otherwise.
bool read_trylock(rwlock_t *rw) {
    int32_t cur = atomic_read(&rw->count);
    if (cur == RW_WRITER || cur < 0)
        return false;
    if (atomic_cmpxchg(&rw->count, cur, cur + 1) != cur)
        return false;
    barrier();
    return true;
}

/// @brief Non-blocking write-lock attempt.
/// @param[in,out] rw The rwlock.
/// @return true if acquired, false otherwise.
bool write_trylock(rwlock_t *rw) {
    if (atomic_cmpxchg(&rw->count, 0, RW_WRITER) != 0)
        return false;
    barrier();
    return true;
}
