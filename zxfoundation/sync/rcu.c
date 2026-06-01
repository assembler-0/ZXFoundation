/// SPDX-License-Identifier: Apache-2.0
/// @file rcu.c
/// @brief minimal RCU implementation.

#include <zxfoundation/sync/rcu.h>
#include <arch/s390x/cpu/lowcore.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/sync/spinlock.h>
#include <arch/s390x/cpu/atomic.h>
#include <arch/s390x/cpu/processor.h>

/// @brief Global lock protecting the RCU callback list.
static spinlock_t   rcu_lock    = SPINLOCK_INIT;
/// @brief Head of the global RCU callback list.
static rcu_head_t  *rcu_cb_head = nullptr;
/// @brief Tail pointer for O(1) appends to the callback list.
static rcu_head_t **rcu_cb_tail = &rcu_cb_head;
/// @brief Global grace-period sequence counter.
static atomic64_t   rcu_gp_seq  = ATOMIC64_INIT(0);

/// @brief Initialize the RCU subsystem.
void rcu_init(void) {
    atomic64_set(&rcu_gp_seq, 0);
    rcu_cb_head = nullptr;
    rcu_cb_tail = &rcu_cb_head;
}

/// @brief Register a callback for deferred execution.
/// @param[in,out] head RCU head embedded in the object.
/// @param[in]     func Function to call after the grace period.
void call_rcu(rcu_head_t *head, void (*func)(rcu_head_t *)) {
    head->func = func;
    head->next = nullptr;

    irqflags_t flags;
    spin_lock_irqsave(&rcu_lock, &flags);
    *rcu_cb_tail = head;
    rcu_cb_tail  = &head->next;
    spin_unlock_irqrestore(&rcu_lock, flags);
}

/// @brief Report that the calling CPU has passed through a quiescent state.
void rcu_report_qs(void) {
    uint64_t gp = (uint64_t)atomic64_read(&rcu_gp_seq);
    percpu_set(rcu_qs_seq, gp);
}

/// @brief Wait for a grace period to complete and drain callbacks.
void synchronize_rcu(void) {
    uint64_t target = (uint64_t)atomic64_add_return(&rcu_gp_seq, 1);

    for (uint16_t i = 0; i < MAX_CPUS; i++) {
        if (zx_lowcore_cpu(i))
            percpu_set_on(i, rcu_gp_seq, target);
    }

    for (uint16_t i = 0; i < MAX_CPUS; i++) {
        if (!zx_lowcore_cpu(i))
            continue;
        while (percpu_get_on(i, rcu_qs_seq) != target)
            arch_cpu_relax();
    }

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
