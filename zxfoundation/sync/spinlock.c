// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/qspinlock.c
//
/// @brief Queue spinlock implementation for z/Architecture.
///
///        FAST PATH (uncontended)
///        =======================
///        If the lock word is 0, a single CS atomically sets locked=1.
///        No queue node is touched.  This is the common case.
///
///        PENDING PATH (one waiter)
///        =========================
///        If locked=1 but tail=0 and pending=0, the second CPU sets
///        pending=1 and spins on the locked byte only.  When the holder
///        clears locked, the pending CPU clears pending and sets locked=1
///        in one CS.  Still no queue node needed.
///
///        QUEUE PATH (two or more waiters)
///        =================================
///        The third and subsequent CPUs encode their per-CPU MCS node
///        pointer into the tail field and link into the MCS queue.
///        Each CPU spins on its own mcs_node_t::locked, which is set by
///        its predecessor when it releases the lock.  This eliminates
///        cache-line bouncing: only the head of the queue observes the
///        lock word changing.
///
///        TAIL ENCODING
///        =============
///        tail[15:4] = cpu_id + 1  (0 means "no tail")
///        tail[3:0]  = nesting index into per-CPU mcs_node array
///        The per-CPU node array is indexed by the nesting depth counter
///        stored in the per-CPU area (percpu_lock_depth).

#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/percpu.h>
#include <arch/s390x/cpu/processor.h>

/// Encode (cpu_id, depth) into the tail field of the lock word.
/// cpu_id is stored as (cpu_id + 1) so that 0 means "no tail".
static inline uint32_t encode_tail(int cpu, uint32_t depth) {
    return (uint32_t)(((cpu + 1) << 4) | (depth & 0xFU)) << _Q_TAIL_OFFSET;
}

static inline int tail_cpu(uint32_t val) {
    return (int)((val >> (_Q_TAIL_OFFSET + 4)) & 0xFFFU) - 1;
}

static inline uint32_t tail_depth(uint32_t val) {
    return (val >> _Q_TAIL_OFFSET) & 0xFU;
}

// ---------------------------------------------------------------------------
// Per-CPU MCS node access
// ---------------------------------------------------------------------------

static inline mcs_node_t *get_mcs_node(int cpu, uint32_t depth) {
    return &percpu_get_on(cpu, lock_nodes)[depth];
}

static inline mcs_node_t *my_mcs_node(uint32_t depth) {
    return &percpu_get(lock_nodes)[depth];
}

bool spin_trylock(spinlock_t *lock) {
    int32_t old = atomic_cmpxchg(&lock->val, 0, (int32_t)_Q_LOCKED_VAL);
    return old == 0;
}

void spin_lock(spinlock_t *lock) {
    // ---- FAST PATH ----
    if (atomic_cmpxchg(&lock->val, 0, (int32_t)_Q_LOCKED_VAL) == 0)
        return;

    int cpu = arch_smp_processor_id();
    uint32_t depth = percpu_get(lock_depth);
    percpu_inc(lock_depth);
    mcs_node_t *node = my_mcs_node(depth);
    node->next = nullptr;
    atomic_set(&node->locked, 0);

    // ---- PENDING PATH: second waiter, no queue yet ----
    int32_t val = atomic_read(&lock->val);
    if (!(val & (int32_t)_Q_TAIL_MASK)) {
        // Try to set pending=1 while locked=1, tail=0.
        int32_t old = val;
        int32_t new = old | (int32_t)_Q_PENDING_VAL;
        if (atomic_cmpxchg(&lock->val, old, new) == old) {
            // Spin until locked byte clears.
            while (atomic_read(&lock->val) & (int32_t)_Q_LOCKED_MASK)
                arch_cpu_relax();
            // Claim lock: clear pending, set locked.
            do {
                old = atomic_read(&lock->val);
                new = (old & ~(int32_t)_Q_PENDING_MASK) | (int32_t)_Q_LOCKED_VAL;
            } while (atomic_cmpxchg(&lock->val, old, new) != old);
            percpu_dec(lock_depth);
            return;
        }
    }

    // ---- QUEUE PATH: encode tail, link into MCS queue ----
    uint32_t tail = encode_tail(cpu, depth);
    int32_t old_val, new_val;
    do {
        old_val = atomic_read(&lock->val);
        new_val = (old_val & ~(int32_t)_Q_TAIL_MASK) | (int32_t)tail;
    } while (atomic_cmpxchg(&lock->val, old_val, new_val) != old_val);

    // Link to previous tail node if one existed.
    uint32_t prev_tail = (uint32_t)old_val & _Q_TAIL_MASK;
    if (prev_tail) {
        mcs_node_t *prev = get_mcs_node(tail_cpu(old_val), tail_depth(old_val));
        mb();
        prev->next = node;
        // Spin on our own cache line until predecessor hands off.
        while (!atomic_read(&node->locked))
            arch_cpu_relax();
    } else {
        // We are the new head; wait for the lock byte to clear.
        while (atomic_read(&lock->val) & (int32_t)_Q_LOCKED_MASK)
            arch_cpu_relax();
    }

    // We are now the queue head.  Claim the lock byte and clear our tail.
    do {
        old_val = atomic_read(&lock->val);
        // Clear tail if it still points to us, set locked=1.
        new_val = (old_val & ~(int32_t)(_Q_TAIL_MASK | _Q_PENDING_MASK))
                  | (int32_t)_Q_LOCKED_VAL;
        if ((uint32_t)old_val & _Q_TAIL_MASK) {
            // Only clear tail if it still encodes us.
            if (((uint32_t)old_val & _Q_TAIL_MASK) == tail)
                new_val = (old_val & ~(int32_t)_Q_TAIL_MASK)
                          | (int32_t)_Q_LOCKED_VAL;
            else
                new_val = old_val | (int32_t)_Q_LOCKED_VAL;
        }
    } while (atomic_cmpxchg(&lock->val, old_val, new_val) != old_val);

    percpu_dec(lock_depth);
}

void spin_unlock(spinlock_t *lock) {
    smp_mb_release();

    int32_t old_val, new_val;
    do {
        old_val = atomic_read(&lock->val);
        new_val = old_val & ~(int32_t)_Q_LOCKED_MASK;
    } while (atomic_cmpxchg(&lock->val, old_val, new_val) != old_val);

    // If there is a next MCS node, wake it.
    // We read the tail field from the value we just wrote.
    uint32_t tail = (uint32_t)new_val & _Q_TAIL_MASK;
    if (tail) {
        int cpu   = tail_cpu(new_val);
        uint32_t d = tail_depth(new_val);
        mcs_node_t *head = get_mcs_node(cpu, d);
        mcs_node_t *next = head->next;
        while (!next) {
            arch_cpu_relax();
            next = head->next;
        }
        mb();
        atomic_set(&next->locked, 1);
    }
}
