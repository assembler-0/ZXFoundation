// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/object/kobject.h
//
/// @brief Base kernel object: reference counting + lifecycle callbacks.
///
///        DESIGN
///        ======
///        kobject is the root type for all reference-counted kernel objects.
///        Subsystems embed it as the first member of their own structs so
///        that container_of() can recover the outer type from a kobject*.
///
///        LIFECYCLE
///        =========
///          kobject_init()   — bind ops, set name, refcount = 1
///          kobject_get()    — increment refcount
///          kobject_put()    — decrement; calls ops->release when zero
///
///        ops->release() is mandatory.  It must free the containing object.
///        It runs with no locks held.
///
///        STATE MACHINE
///        =============
///          UNINITIALIZED → ALIVE (after kobject_init)
///          ALIVE         → DEAD  (when refcount hits zero, before release)
///
///        Accessing a DEAD kobject is a bug.  The state field is checked
///        in debug paths only — it is not a security boundary.

#pragma once

#include <zxfoundation/object/kref.h>
#include <zxfoundation/spinlock.h>

/// @brief Kobject lifecycle state.
typedef enum {
    KOBJECT_UNINITIALIZED = 0,
    KOBJECT_ALIVE,
    KOBJECT_DEAD,
} kobject_state_t;

struct kobject;

/// @brief Operations table for a kobject type.
typedef struct kobject_ops {
    /// @brief Called when the last reference is dropped.
    ///        Must free the containing object.  No locks held.
    void (*release)(struct kobject *obj);
} kobject_ops_t;

/// @brief Base kernel object.  Embed as first member of subsystem structs.
typedef struct kobject {
    kref_t              ref;
    const kobject_ops_t *ops;
    kobject_state_t     state;
    const char          *name;   ///< Static string; not owned by kobject.
} kobject_t;

/// @brief Recover the containing struct from an embedded kobject pointer.
#define kobject_container(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

/// @brief Initialize a kobject.  Sets refcount to 1.
/// @param obj   The kobject to initialize.
/// @param ops   Operations table (must not be NULL; release is mandatory).
/// @param name  Static name string for diagnostics.
void kobject_init(kobject_t *obj, const kobject_ops_t *ops, const char *name);

/// @brief Increment the reference count.
/// @return obj (for chaining).
kobject_t *kobject_get(kobject_t *obj);

/// @brief Decrement the reference count.  Calls ops->release if zero.
void kobject_put(kobject_t *obj);

/// @brief Increment only if the object is still alive (non-zero refcount).
///        Safe under RCU — use when looking up objects in lockless tables.
/// @return true if the reference was acquired.
bool kobject_get_unless_dead(kobject_t *obj);
