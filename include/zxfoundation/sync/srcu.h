/// SPDX-License-Identifier: Apache-2.0
/// @file srcu.h
/// @brief Sleepable RCU (SRCU).

#pragma once

#include <arch/s390x/cpu/atomic.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/sync/rcu.h>

/// @brief Per-CPU read counters for one SRCU domain.
typedef struct {
    atomic_t c[2]; ///< Two counter slots for the two SRCU phases.
} srcu_percpu_t;

/// @brief SRCU domain structure.
typedef struct srcu_struct {
    int             idx;                        ///< Currently active slot (0 or 1).
    srcu_percpu_t   pcpu[MAX_CPUS];             ///< Per-CPU read counters.
    atomic_t        gp_seq;                     ///< Grace-period counter.
} srcu_struct_t;

/// @name SRCU Initialization
/// @{
/// @brief Static initializer for an SRCU domain.
#define SRCU_INIT(name) {                       \
    .idx    = 0,                                \
    .gp_seq = ATOMIC_INIT(0),                   \
}

/// @brief Define and initialize a static SRCU domain.
#define DEFINE_SRCU(name)   srcu_struct_t name = SRCU_INIT(name)

/// @brief Initialize an SRCU domain at runtime.
/// @param[in,out] s The SRCU domain to initialize.
void srcu_init(srcu_struct_t *s);
/// @}

/// @brief Enter an SRCU read-side critical section.
/// @param[in,out] s The SRCU domain.
/// @return An index to be passed to the matching srcu_read_unlock().
/// @note Readers may sleep or block within the critical section.
int srcu_read_lock(srcu_struct_t *s);

/// @brief Exit an SRCU read-side critical section.
/// @param[in,out] s   The SRCU domain.
/// @param[in]     idx The value returned by the matching srcu_read_lock().
void srcu_read_unlock(srcu_struct_t *s, int idx);

/// @brief Wait for all pre-existing SRCU readers to complete.
/// @param[in,out] s The SRCU domain.
/// @note This function may sleep and MUST NOT be called from interrupt context.
void synchronize_srcu(srcu_struct_t *s);

/// @brief Register a callback to be called after the next SRCU grace period.
/// @param[in,out] s    The SRCU domain.
/// @param[in,out] head RCU head embedded in the object to be freed.
/// @param[in]     func Callback function.
void call_srcu(srcu_struct_t *s, rcu_head_t *head,
               void (*func)(rcu_head_t *));
