/// SPDX-License-Identifier: Apache-2.0
/// @file srcu.c
/// @brief Sleepable RCU (SRCU) implementation.

#include <zxfoundation/sync/srcu.h>
#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/sync/spinlock.h>
#include <arch/s390x/cpu/processor.h>
#include <lib/string.h>

/// @brief Initialize an SRCU domain.
/// @param[in,out] s The SRCU domain.
void srcu_init(srcu_struct_t *s) {
    s->idx = 0;
    atomic_set(&s->gp_seq, 0);
    for (uint16_t i = 0; i < MAX_CPUS; i++) {
        atomic_set(&s->pcpu[i].c[0], 0);
        atomic_set(&s->pcpu[i].c[1], 0);
    }
}

/// @brief Enter an SRCU read-side critical section.
/// @param[in,out] s The SRCU domain.
/// @return The index of the counter slot used.
int srcu_read_lock(srcu_struct_t *s) {
    int idx = s->idx;
    int cpu = arch_smp_processor_id();
    // Ensure the read of s->idx is not reordered with the counter increment.
    barrier();
    atomic_inc(&s->pcpu[cpu].c[idx]);
    barrier();
    return idx;
}

/// @brief Exit an SRCU read-side critical section.
/// @param[in,out] s   The SRCU domain.
/// @param[in]     idx The index returned by srcu_read_lock().
void srcu_read_unlock(srcu_struct_t *s, int idx) {
    int cpu = arch_smp_processor_id();
    // Ensure all accesses in the critical section are visible before decrement.
    smp_mb_release();
    atomic_dec(&s->pcpu[cpu].c[idx]);
}

/// @brief Sum all per-CPU counters for a specific slot.
/// @param[in] s   The SRCU domain.
/// @param[in] idx Slot index (0 or 1).
/// @return Total number of active readers for that slot across all CPUs.
static int32_t srcu_readers(const srcu_struct_t *s, int idx) {
    int32_t sum = 0;
    for (uint16_t i = 0; i < MAX_CPUS; i++) {
        if (zx_lowcore_cpu(i))
            sum += atomic_read((atomic_t *)&s->pcpu[i].c[idx]);
    }
    return sum;
}

/// @brief Wait for a grace period to complete.
/// @param[in,out] s The SRCU domain.
void synchronize_srcu(srcu_struct_t *s) {
    int old_idx = s->idx;
    int new_idx = 1 - old_idx;

    // Full barrier to ensure all prior updates are visible before flipping.
    smp_mb();
    s->idx = new_idx;
    smp_mb();

    // Wait for readers from the old phase to drain.
    // TODO: replace with sleeping wait when scheduler is available.
    while (srcu_readers(s, old_idx) != 0)
        arch_cpu_relax();

    // Grace period complete.
    atomic_inc(&s->gp_seq);
}

/// @brief Register a callback for deferred execution.
/// @param[in,out] s    The SRCU domain.
/// @param[in,out] head RCU head node.
/// @param[in]     func Callback function.
/// @note Current implementation synchronizes immediately.
void call_srcu(srcu_struct_t *s, rcu_head_t *head,
               void (*func)(rcu_head_t *)) {
    // For now, simple blocking implementation.
    synchronize_srcu(s);
    head->func = func;
    head->next = nullptr;
    func(head);
}
