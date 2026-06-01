/// SPDX-License-Identifier: Apache-2.0
/// @file kref.h
/// @brief Atomic reference counter for kernel objects.

#pragma once

#include <arch/s390x/cpu/atomic.h>

/// @brief Atomic reference counter.
typedef struct {
    atomic_t refcount; ///< Underlying atomic counter.
} kref_t;

/// @brief Initialize a kref to 1.
/// @param[in,out] ref The kref to initialize.
/// @note The caller is assumed to hold the initial reference.
static inline void kref_init(kref_t *ref) {
    atomic_set(&ref->refcount, 1);
}

/// @brief Increment the reference count.
/// @param[in,out] ref The kref.
/// @warning Must only be called when the caller already holds a reference
///          to ensure the object is not freed during the call.
static inline void kref_get(kref_t *ref) {
    atomic_inc(&ref->refcount);
}

/// @brief Decrement the reference count and potentially release the object.
/// @param[in,out] ref     The kref.
/// @param[in]     release Callback function called when the count hits zero.
/// @return true if the object was released (count hit zero), false otherwise.
/// @note If the count reaches zero, release(ref) is called.
static inline bool kref_put(kref_t *ref, void (*release)(kref_t *)) {
    if (atomic_dec_and_test(&ref->refcount)) {
        release(ref);
        return true;
    }
    return false;
}

/// @brief Increment the reference count only if it is currently non-zero.
/// @param[in,out] ref The kref.
/// @return true if the reference was successfully acquired.
/// @note This is safe for lockless lookups under RCU protection where
///       an object may be concurrently released.
static inline bool kref_get_unless_zero(kref_t *ref) {
    int32_t cur;
    do {
        cur = atomic_read(&ref->refcount);
        if (cur == 0)
            return false;
    } while (atomic_cmpxchg(&ref->refcount, cur, cur + 1) != cur);
    return true;
}
