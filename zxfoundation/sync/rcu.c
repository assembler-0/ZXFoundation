// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sync/rcu.c
//
/// @brief Classic non-preemptive RCU for ZXFoundation.
///
///        DESIGN
///        ======
///        In a non-preemptive kernel, a quiescent state (QS) occurs whenever
///        a CPU is not inside an rcu_read_lock() section.  Because the kernel
///        is non-preemptive, any context switch or explicit QS report is
///        sufficient.
///
///        We track quiescence with a per-CPU bit in a global bitmask.
///        synchronize_rcu() waits until every online CPU has passed through
///        at least one quiescent state since the grace period began, then
///        drains the callback list.
///
///        GRACE PERIOD SEQUENCE
///        =====================
///        1. Snapshot the set of online CPUs into gp_cpumask.
///        2. Increment gp_seq (grace-period counter).
///        3. Broadcast a QS request to all CPUs (sets their rcu_qs_seq).
///        4. Spin until every CPU in gp_cpumask has reported QS
///           (rcu_qs_seq == gp_seq on each CPU's per-CPU area).
///        5. Drain the callback list.
///
///        CALLBACK LIST
///        =============
///        call_rcu() appends to a singly-linked list protected by a
///        spinlock.  Callbacks are drained under the same lock after the
///        grace period completes.

#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/sync/spinlock.h>
#include <arch/s390x/cpu/atomic.h>
#include <arch/s390x/cpu/processor.h>

// ---------------------------------------------------------------------------
// Global RCU state
// ---------------------------------------------------------------------------

static spinlock_t   rcu_lock    = SPINLOCK_INIT;
static rcu_head_t  *rcu_cb_head = nullptr;
static rcu_head_t **rcu_cb_tail = &rcu_cb_head;
static atomic64_t   rcu_gp_seq  = ATOMIC64_INIT(0);

// ---------------------------------------------------------------------------
// rcu_init
// ---------------------------------------------------------------------------

void rcu_init(void) {
    atomic64_set(&rcu_gp_seq, 0);
    rcu_cb_head = nullptr;
    rcu_cb_tail = &rcu_cb_head;
}

// ---------------------------------------------------------------------------
// call_rcu
// ---------------------------------------------------------------------------

void call_rcu(rcu_head_t *head, void (*func)(rcu_head_t *)) {
    head->func = func;
    head->next = nullptr;

    irqflags_t flags;
    spin_lock_irqsave(&rcu_lock, &flags);
    *rcu_cb_tail = head;
    rcu_cb_tail  = &head->next;
    spin_unlock_irqrestore(&rcu_lock, flags);
}

// ---------------------------------------------------------------------------
// rcu_report_qs — called by each CPU to report a quiescent state
// ---------------------------------------------------------------------------

void rcu_report_qs(void) {
    uint64_t gp = (uint64_t)atomic64_read(&rcu_gp_seq);
    percpu_set(rcu_qs_seq, gp);
}

// ---------------------------------------------------------------------------
// synchronize_rcu
// ---------------------------------------------------------------------------

void synchronize_rcu(void) {
    // Advance the grace-period counter.
    uint64_t target = (uint64_t)atomic64_add_return(&rcu_gp_seq, 1);

    // Broadcast the new GP sequence to all online CPUs.
    for (uint16_t i = 0; i < MAX_CPUS; i++) {
        if (percpu_areas[i])
            percpu_set_on(i, rcu_gp_seq, target);
    }

    // Wait until every online CPU has passed through a QS.
    // A CPU reports QS by writing rcu_qs_seq == rcu_gp_seq.
    // On a non-preemptive kernel, any code path that calls rcu_report_qs()
    // (e.g. the idle loop, scheduler tick) satisfies this.
    for (uint16_t i = 0; i < MAX_CPUS; i++) {
        if (!percpu_areas[i])
            continue;
        while (percpu_get_on(i, rcu_qs_seq) != target)
            arch_cpu_relax();
    }

    // Drain callbacks.
    irqflags_t flags;
    spin_lock_irqsave(&rcu_lock, &flags);
    rcu_head_t *list = rcu_cb_head;
    rcu_cb_head = nullptr;
    rcu_cb_tail = &rcu_cb_head;
    spin_unlock_irqrestore(&rcu_lock, flags);

    while (list) {
        rcu_head_t *next = list->next;
        list->func(list);
        list = next;
    }
}
