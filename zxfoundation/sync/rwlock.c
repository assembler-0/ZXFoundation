// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sync/rwlock.c

#include <zxfoundation/sync/rwlock.h>
#include <arch/s390x/cpu/processor.h>

void read_lock(rwlock_t *rw) {
    int32_t cur;
    do {
        do {
            cur = atomic_read(&rw->count);
        } while (cur == RW_WRITER && (cpu_relax(), true));
        // CAS: increment reader count only if still not RW_WRITER.
    } while (atomic_cmpxchg(&rw->count, cur, cur + 1) != cur);
    barrier(); // acquire
}

void read_unlock(rwlock_t *rw) {
    smp_mb_release();
    atomic_add_return(&rw->count, -1);
}

void write_lock(rwlock_t *rw) {
    while (atomic_cmpxchg(&rw->count, 0, RW_WRITER) != 0)
        cpu_relax();
    barrier(); // acquire
}

void write_unlock(rwlock_t *rw) {
    smp_mb_release();
    atomic_set(&rw->count, 0);
}

bool read_trylock(rwlock_t *rw) {
    int32_t cur = atomic_read(&rw->count);
    if (cur == RW_WRITER || cur < 0)
        return false;
    if (atomic_cmpxchg(&rw->count, cur, cur + 1) != cur)
        return false;
    barrier();
    return true;
}

bool write_trylock(rwlock_t *rw) {
    if (atomic_cmpxchg(&rw->count, 0, RW_WRITER) != 0)
        return false;
    barrier();
    return true;
}
