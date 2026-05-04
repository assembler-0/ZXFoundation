// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sync/rcu.c
//
// Non-preemptive RCU.  A grace period is complete when every CPU has
// executed at least one instruction outside a read-side critical section.
// Without a scheduler, synchronize_rcu() is a full SMP barrier — on a
// single-CPU kernel that is sufficient.  When SMP is added, this becomes
// a per-CPU quiescent-state counter check.

#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/spinlock.h>

static spinlock_t  rcu_lock    = SPINLOCK_INIT;
static rcu_head_t *rcu_pending = nullptr;   // pending callback list

void rcu_init(void) {
    spin_lock_init(&rcu_lock);
    rcu_pending = nullptr;
}

void call_rcu(rcu_head_t *head, void (*func)(rcu_head_t *)) {
    head->func = func;

    irqflags_t flags;
    spin_lock_irqsave(&rcu_lock, &flags);
    head->next   = rcu_pending;
    rcu_pending  = head;
    spin_unlock_irqrestore(&rcu_lock, flags);
}

void synchronize_rcu(void) {
    smp_mb();

    irqflags_t flags;
    spin_lock_irqsave(&rcu_lock, &flags);
    rcu_head_t *list = rcu_pending;
    rcu_pending = nullptr;
    spin_unlock_irqrestore(&rcu_lock, flags);

    while (list) {
        rcu_head_t *next = list->next;
        list->func(list);
        list = next;
    }
}
