// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sync/srcu.c

#include <zxfoundation/sync/srcu.h>
#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/sync/spinlock.h>
#include <arch/s390x/cpu/processor.h>
#include <lib/string.h>

void srcu_init(srcu_struct_t *s) {
    s->idx = 0;
    atomic_set(&s->gp_seq, 0);
    for (uint16_t i = 0; i < MAX_CPUS; i++) {
        atomic_set(&s->pcpu[i].c[0], 0);
        atomic_set(&s->pcpu[i].c[1], 0);
    }
}

int srcu_read_lock(srcu_struct_t *s) {
    int idx = s->idx;
    int cpu = arch_smp_processor_id();
    // Acquire barrier: ensure the idx read is not reordered past the
    // counter increment.  On s390x TSO a compiler barrier suffices.
    barrier();
    atomic_inc(&s->pcpu[cpu].c[idx]);
    barrier();
    return idx;
}

void srcu_read_unlock(srcu_struct_t *s, int idx) {
    int cpu = arch_smp_processor_id();
    // Release barrier: all accesses inside the read-side section must be
    // visible before we decrement the counter.
    smp_mb_release();
    atomic_dec(&s->pcpu[cpu].c[idx]);
}

/// @brief Sum all per-CPU counters for slot idx.
static int32_t srcu_readers(const srcu_struct_t *s, int idx) {
    int32_t sum = 0;
    for (uint16_t i = 0; i < MAX_CPUS; i++) {
        if (percpu_areas[i])
            sum += atomic_read((atomic_t *)&s->pcpu[i].c[idx]);
    }
    return sum;
}

void synchronize_srcu(srcu_struct_t *s) {
    // Flip the active slot so new readers use the other counter.
    int old_idx = s->idx;
    int new_idx = 1 - old_idx;

    // Full barrier before flipping: all prior stores must be visible.
    smp_mb();
    s->idx = new_idx;
    smp_mb();

    // Wait for all readers still using old_idx to finish.
    // This may spin; in a future preemptive kernel this would sleep.
    while (srcu_readers(s, old_idx) != 0)
        arch_cpu_relax();

    atomic_inc(&s->gp_seq);
}

void call_srcu(srcu_struct_t *s, rcu_head_t *head,
               void (*func)(rcu_head_t *)) {
    // For now, synchronize immediately then invoke.
    // A deferred callback queue can be added when the scheduler exists.
    synchronize_srcu(s);
    head->func = func;
    head->next = nullptr;
    func(head);
}
