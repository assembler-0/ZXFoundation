// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sync/rcu_types.h
//
/// @brief Core RCU types and macros to break include cycles.

#pragma once

#include <arch/s390x/cpu/atomic.h>

/// @brief RCU callback node.
///
/// Objects that need to be freed after a grace period must embed this structure.
typedef struct rcu_head {
    struct rcu_head *next;                  ///< Linkage into the global callback list.
    void (*func)(struct rcu_head *head);    ///< Callback function.
} rcu_head_t;

/// @brief Safely publish a pointer to a newly initialized object.
/// @param[out] p The pointer to update.
/// @param[in]  v The new value.
/// @note The smp_mb() ensures all stores to *v are visible before
///       the pointer itself becomes visible to readers.
#define rcu_assign_pointer(p, v)    \
    do {                            \
        smp_mb();                   \
        (p) = (v);                  \
    } while (0)

/// @brief Safely read an RCU-protected pointer.
/// @param[in] p The pointer to read.
/// @return The value of the pointer.
/// @note The compiler barrier prevents the compiler from re-reading the
///       pointer after it has been dereferenced.
#define rcu_dereference(p)          \
    ({                              \
        __typeof__(p) _v = (p);     \
        barrier();                  \
        _v;                         \
    })
