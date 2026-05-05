// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/object/kref.h
//
/// @brief Atomic reference counter for kernel objects.
///
///        DESIGN
///        ======
///        kref wraps atomic_t with a strict lifecycle contract:
///          - kref_init()  sets refcount to 1 (the initial owner holds a ref).
///          - kref_get()   increments; must only be called when refcount > 0.
///          - kref_put()   decrements; calls release() when it hits zero.
///
///        kref_get() on a zero-refcount object is a use-after-free bug.
///        kref_get_unless_zero() is the safe variant for lockless lookups
///        (e.g. RCU-protected hash tables) where the object may be
///        concurrently freed.
///
///        The release callback runs with no locks held.  It is responsible
///        for freeing the containing object.

#pragma once

#include <arch/s390x/cpu/atomic.h>

typedef struct {
    atomic_t refcount;
} kref_t;

/// @brief Initialize a kref to 1 (caller holds the initial reference).
static inline void kref_init(kref_t *ref) {
    atomic_set(&ref->refcount, 1);
}

/// @brief Increment the reference count.
///        Must only be called when the caller already holds a reference.
static inline void kref_get(kref_t *ref) {
    atomic_inc(&ref->refcount);
}

/// @brief Decrement the reference count.
///        Calls release(ref) if the count reaches zero.
/// @return true if the object was released.
static inline bool kref_put(kref_t *ref, void (*release)(kref_t *)) {
    if (atomic_dec_and_test(&ref->refcount)) {
        release(ref);
        return true;
    }
    return false;
}

/// @brief Increment only if the current count is non-zero.
///        Safe for lockless lookups under RCU.
/// @return true if the reference was acquired.
static inline bool kref_get_unless_zero(kref_t *ref) {
    int32_t cur;
    do {
        cur = atomic_read(&ref->refcount);
        if (cur == 0)
            return false;
    } while (atomic_cmpxchg(&ref->refcount, cur, cur + 1) != cur);
    return true;
}
