/// SPDX-License-Identifier: Apache-2.0
/// @file koms.h
/// @brief Kernel Object Management System (KOMS).

#pragma once

#include <zxfoundation/object/kref.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/slab.h>
#include <zxfoundation/time/ktime.h>
#include <lib/list.h>

typedef struct kobj_type kobj_type_t;
typedef struct kobj_ns kobj_ns_t;
typedef struct kobj_attr kobj_attr_t;
typedef struct kobj_event kobj_event_t;
typedef struct kobj_listener kobj_listener_t;
typedef struct kobject kobject_t;

/// @name KOMS Type IDs
/// @{
#define KOBJ_TYPE_INVALID       0U
#define KOBJ_TYPE_GENERIC       1U
#define KOBJ_TYPE_VMA           2U
#define KOBJ_TYPE_IRQ           3U
#define KOBJ_TYPE_DEVICE        4U
#define KOBJ_TYPE_DRIVER        5U
#define KOBJ_TYPE_BUS           6U
#define KOBJ_TYPE_TASK          7U
#define KOBJ_TYPE_FILE          8U
#define KOBJ_TYPE_INODE         9U
#define KOBJ_TYPE_SOCKET        10U
#define KOBJ_TYPE_TIMER         11U
#define KOBJ_TYPE_WORKQUEUE     12U
#define KOBJ_TYPE_USER_BASE     256U
/// @}

/// @name KOMS Object Flags
/// @{
/// @brief Object was allocated by koms_alloc() and must be freed by koms_free().
#define KOBJ_FLAG_KOMS_ALLOC    (1U << 0)
/// @brief Object is registered in a namespace.
#define KOBJ_FLAG_IN_NS         (1U << 1)
/// @brief Object has at least one attribute attached.
#define KOBJ_FLAG_HAS_ATTRS     (1U << 2)
/// @brief Object has at least one event listener attached.
#define KOBJ_FLAG_HAS_LISTENERS (1U << 3)
/// @brief Object is frozen: no new references may be acquired.
#define KOBJ_FLAG_FROZEN        (1U << 4)
/// @}

/// @brief KOMS Object State
typedef enum {
    KOBJECT_UNINITIALIZED = 0, ///< Object not yet initialized.
    KOBJECT_ALIVE,            ///< Object is active and may be referenced.
    KOBJECT_DEAD,             ///< Object is being destroyed.
} kobject_state_t;

/// @brief KOMS Event Types
typedef enum {
    KOBJ_EVENT_NONE = 0,
    KOBJ_EVENT_CREATED,        ///< Object has been initialized.
    KOBJ_EVENT_DESTROYED,      ///< Object's last reference was dropped.
    KOBJ_EVENT_STATE_CHANGE,   ///< Object state transitioned.
    KOBJ_EVENT_ATTR_SET,       ///< An attribute was modified.
    KOBJ_EVENT_CHILD_ADD,      ///< A child was added to the hierarchy.
    KOBJ_EVENT_CHILD_REMOVE,   ///< A child was removed from the hierarchy.
    KOBJ_EVENT_USER_BASE = 256,
} kobj_event_type_t;

/// @brief KOMS Event Payload
typedef union {
    struct {
        kobject_state_t old_state;
        kobject_state_t new_state;
    } state_change;

    struct {
        const kobj_attr_t *attr;
    } attr_set;

    struct {
        kobject_t *child;
    } child;

    uint64_t raw[4];
} kobj_event_payload_t;

/// @brief KOMS Event Structure
struct kobj_event {
    kobj_event_type_t type;     ///< Type of event.
    kobject_t *source;          ///< Object that generated the event.
    kobj_event_payload_t payload; ///< Event-specific data.
};

/// @brief KOMS Event Listener Callback
/// @param[in] event The event that occurred.
/// @param[in] data  User-provided pointer.
typedef void (*kobj_event_fn_t)(const kobj_event_t *event, void *data);

/// @brief KOMS Event Listener
struct kobj_listener {
    list_head_t node;           ///< Linkage into kobject::listeners.
    kobj_event_fn_t fn;         ///< Callback function.
    void *data;                 ///< User-provided data for the callback.
    /// @brief Bitmask of kobj_event_type_t values to receive; 0 = all events.
    uint32_t event_mask;
};

/// @brief KOMS Attribute Getter
/// @param[in]  obj   Target kobject.
/// @param[in]  attr  The attribute being accessed.
/// @param[out] buf   Output buffer for the attribute value (string).
/// @param[in]  size  Capacity of the output buffer.
/// @return Number of bytes written to buf, or negative errno on failure.
typedef int (*kobj_attr_get_fn_t)(kobject_t *obj, const kobj_attr_t *attr,
                                  char *buf, size_t size);

/// @brief KOMS Attribute Setter
/// @param[in] obj   Target kobject.
/// @param[in] attr  The attribute being accessed.
/// @param[in] buf   Input buffer containing the new value (string).
/// @param[in] size  Length of the input buffer (excluding NUL).
/// @return Number of bytes consumed from buf, or negative errno on failure.
typedef int (*kobj_attr_set_fn_t)(kobject_t *obj, const kobj_attr_t *attr,
                                  const char *buf, size_t size);

/// @brief KOMS Attribute Descriptor
struct kobj_attr {
    list_head_t node;           ///< Linkage into kobject::attrs.
    const char *name;           ///< Attribute name (unique per kobject).
    kobj_attr_get_fn_t get;     ///< Getter callback. May be nullptr (write-only).
    kobj_attr_set_fn_t set;     ///< Setter callback. May be nullptr (read-only).
};

/// @brief KOMS Object Operations
typedef struct kobject_ops {
    /// @brief Called when the last reference is dropped.
    /// @param[in] obj The object to release.
    /// @note Must free the containing object if it was dynamically allocated.
    ///       Called with no locks held.
    void (*release)(kobject_t *obj);
} kobject_ops_t;

/// @brief KOMS Type Operations
typedef struct {
    /// @brief Called after koms_alloc() zeroes the object.
    /// @param[in] obj The object to initialize.
    void (*init)(kobject_t *obj);

    /// @brief Called just before ops->release() when the last reference is dropped.
    /// @param[in] obj The object to destroy.
    void (*destroy)(kobject_t *obj);

    /// @brief Called when the object is added to a namespace.
    /// @param[in] obj The object being added.
    /// @param[in] ns  The namespace it is being added to.
    void (*ns_add)(kobject_t *obj, kobj_ns_t *ns);

    /// @brief Called when the object is removed from a namespace.
    /// @param[in] obj The object being removed.
    /// @param[in] ns  The namespace it was removed from.
    void (*ns_remove)(kobject_t *obj, kobj_ns_t *ns);
} kobj_type_ops_t;

/// @brief KOMS Type Descriptor
struct kobj_type {
    uint32_t type_id;             ///< Globally unique type ID.
    const char *name;             ///< Human-readable name.
    size_t obj_size;              ///< Size of the containing structure.
    kmem_cache_t *cache;          ///< Slab cache. If nullptr, kmalloc is used.
    const kobject_ops_t *kobj_ops; ///< Mandatory: provides the release() handler.
    const kobj_type_ops_t *type_ops; ///< Optional lifecycle hooks.
    list_head_t registry_node;    ///< Linkage into global type registry.
};

#define KOBJ_NS_BUCKETS 64U

typedef struct {
    list_head_t chain;
} kobj_ns_bucket_t;

/// @brief KOMS Namespace
struct kobj_ns {
    kobject_t *owner;            ///< Object that owns this namespace.
    kobj_ns_t *parent;           ///< Parent namespace.
    list_head_t children;        ///< List of child namespaces.
    list_head_t sibling;         ///< Linkage into parent's children list.
    spinlock_t write_lock;       ///< Protects membership and hierarchy.
    kobj_ns_bucket_t buckets[KOBJ_NS_BUCKETS]; ///< Hash table for object lookup.
    uint32_t count;              ///< Number of objects in the namespace.
    const char *name;            ///< Name of the namespace.
};

/// @brief Base structure for all kernel objects.
///
/// kobject_t provides the fundamental mechanisms for lifetime management
/// (refcounting), naming, hierarchy, and event notification. It is usually
/// embedded in a larger structure.
struct kobject {
    kref_t ref;                 ///< Reference count.
    const kobject_ops_t *ops;    ///< Object operations (release).
    kobject_state_t state;       ///< Lifecycle state.
    const char *name;           ///< Object name.

    // KOMS extensions
    uint32_t type_id;           ///< Maps to a kobj_type_t.
    uint32_t flags;             ///< KOBJ_FLAG_* bitmask.
    kobject_t *parent;          ///< Parent in the hierarchy.
    list_head_t sibling;        ///< Linkage into parent's children list.
    list_head_t children;       ///< List of child kobjects.
    list_head_t attrs;          ///< List of kobj_attr_t.
    list_head_t listeners;      ///< List of kobj_listener_t.
    rcu_head_t rcu;             ///< RCU callback head for deferred freeing.
    spinlock_t lock;            ///< Protects hierarchy and flags.
    kobj_ns_t *ns;              ///< Namespace this object belongs to.
    list_head_t ns_node;        ///< Linkage into namespace hash bucket.
    ktime_t created_at;         ///< Timestamp of initialization.
};

/// @brief Recover the containing struct from an embedded kobject pointer.
/// @param ptr    Pointer to the embedded kobject_t.
/// @param type   Type of the containing structure.
/// @param member Name of the kobject_t member in the containing structure.
#define kobject_container(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

/// @brief The root KOMS namespace.
extern kobj_ns_t koms_root_ns;

/// @brief Initialize KOMS.
/// @note Must be called after kmalloc_init() and before any kobject use.
void koms_init(void);

/// @brief Register a type descriptor.
/// @param[in,out] type The type to register. type_id must be unique.
void koms_type_register(kobj_type_t *type);

/// @brief Look up a type by ID.
/// @param[in] type_id The ID to look for.
/// @return Pointer to the type descriptor, or nullptr if not found.
/// @note This function is lockless and safe from any context.
const kobj_type_t *koms_type_lookup(uint32_t type_id);

/// @brief Allocate and zero an object of the given type.
/// @param[in] type The type descriptor.
/// @param[in] gfp  Allocation flags.
/// @return Pointer to the embedded kobject_t, or nullptr on OOM.
/// @note Returns an object with refcount = 0 and KOBJ_FLAG_KOMS_ALLOC set.
///       Must be followed by koms_init_obj().
kobject_t *koms_alloc(const kobj_type_t *type, gfp_t gfp);

/// @brief Initialize a kobject.
/// @param[in,out] obj    The kobject to initialize.
/// @param[in]     type   Type descriptor (optional).
/// @param[in]     name   Object name (must be stable).
/// @param[in,out] parent Parent kobject (optional).
/// @note Sets refcount = 1, sets state to ALIVE, and fires KOBJ_EVENT_CREATED.
///       If parent is provided, adds obj as a child of parent.
void koms_init_obj(kobject_t *obj, const kobj_type_t *type,
                   const char *name, kobject_t *parent);

/// @brief Increment the reference count.
/// @param[in,out] obj The kobject.
/// @return The same pointer (for chaining).
kobject_t *koms_get(kobject_t *obj);

/// @brief Decrement the reference count.
/// @param[in,out] obj The kobject.
/// @note When refcount reaches zero, fires KOBJ_EVENT_DESTROYED, calls
///       type_ops->destroy() and then ops->release().
void koms_put(kobject_t *obj);

/// @brief Safely acquire a reference to a potentially dying object.
/// @param[in,out] obj The kobject.
/// @return true if the reference was successfully acquired.
/// @note Safe to call under RCU. Fails if the object is dead, frozen, or
///       has a refcount of zero.
bool koms_get_unless_dead(kobject_t *obj);

/// @brief Freeze an object, preventing new references from being acquired.
/// @param[in,out] obj The kobject.
/// @note Existing references remain valid. Subsequent koms_get_unless_dead()
///       calls will fail.
void koms_freeze(kobject_t *obj);

/// @brief Free a dynamically allocated kobject.
/// @param[in,out] obj The kobject to free.
/// @note Must only be called from an object's release() operation if it
///       was allocated via koms_alloc().
void koms_free(kobject_t *obj);

/// @brief Initialize a KOMS namespace.
/// @param[in,out] ns     The namespace structure to initialize.
/// @param[in]     name   Name of the namespace.
/// @param[in,out] parent Parent namespace. If nullptr, attaches to koms_root_ns.
/// @param[in,out] owner  Object that owns this namespace (optional).
void koms_ns_init(kobj_ns_t *ns, const char *name,
                  kobj_ns_t *parent, kobject_t *owner);

/// @brief Register an object in a namespace.
/// @param[in,out] ns  The namespace.
/// @param[in,out] obj The object to add. obj->name must be set.
/// @return 0 on success, -1 if the name already exists in the namespace.
int koms_ns_add(kobj_ns_t *ns, kobject_t *obj);

/// @brief Remove an object from its current namespace.
/// @param[in,out] obj The object to remove.
void koms_ns_remove(kobject_t *obj);

/// @brief Look up an object by name in a namespace.
/// @param[in] ns   The namespace.
/// @param[in] name The name to search for.
/// @return Pointer to the kobject, or nullptr if not found.
/// @note This is an RCU read-side operation. Does NOT increment refcount.
///       Must be called within rcu_read_lock().
kobject_t *koms_ns_find(kobj_ns_t *ns, const char *name);

/// @brief Look up an object by name and acquire a reference.
/// @param[in] ns   The namespace.
/// @param[in] name The name to search for.
/// @return Pointer to the kobject with a bumped refcount, or nullptr.
kobject_t *koms_ns_find_get(kobj_ns_t *ns, const char *name);

/// @brief Attach an attribute to a kobject.
/// @param[in,out] obj  The kobject.
/// @param[in,out] attr The attribute descriptor.
/// @return 0 on success, -1 if an attribute with the same name exists.
int koms_attr_add(kobject_t *obj, kobj_attr_t *attr);

/// @brief Detach an attribute from a kobject.
/// @param[in,out] obj  The kobject.
/// @param[in,out] attr The attribute descriptor.
void koms_attr_remove(kobject_t *obj, kobj_attr_t *attr);

/// @brief Read an attribute's value into a buffer.
/// @param[in,out] obj  The kobject.
/// @param[in]     name Attribute name.
/// @param[out]    buf  Output buffer.
/// @param[in]     size Buffer capacity.
/// @return Bytes written to buf, or -1 on error.
int koms_attr_get(kobject_t *obj, const char *name, char *buf, size_t size);

/// @brief Update an attribute's value.
/// @param[in,out] obj  The kobject.
/// @param[in]     name Attribute name.
/// @param[in]     buf  Input buffer (string).
/// @param[in]     size Length of input.
/// @return Bytes consumed, or -1 on error.
/// @note Fires KOBJ_EVENT_ATTR_SET on success.
int koms_attr_set(kobject_t *obj, const char *name, const char *buf, size_t size);

/// @brief Register an event listener for a kobject.
/// @param[in,out] obj      The kobject.
/// @param[in,out] listener The listener structure.
void koms_listener_add(kobject_t *obj, kobj_listener_t *listener);

/// @brief Unregister an event listener.
/// @param[in,out] obj      The kobject.
/// @param[in,out] listener The listener to remove.
void koms_listener_remove(kobject_t *obj, kobj_listener_t *listener);

/// @brief Dispatch an event to listeners and propagate to parents.
/// @param[in,out] obj   The source kobject.
/// @param[in,out] event The event to dispatch.
void koms_event_dispatch(kobject_t *obj, kobj_event_t *event);

/// @brief Construct and dispatch a simple event.
/// @param[in,out] obj  The source kobject.
/// @param[in]     type Event type.
static inline void koms_event_fire(kobject_t *obj, kobj_event_type_t type) {
    kobj_event_t ev = {.type = type, .source = obj};
    koms_event_dispatch(obj, &ev);
}

/// @brief Link a child kobject to a parent.
/// @param[in,out] parent The parent kobject.
/// @param[in,out] child  The child kobject to add.
/// @note Fires KOBJ_EVENT_CHILD_ADD on the parent.
void koms_child_add(kobject_t *parent, kobject_t *child);

/// @brief Unlink a child kobject from its parent.
/// @param[in,out] child The child to remove.
/// @note Fires KOBJ_EVENT_CHILD_REMOVE on the parent.
void koms_child_remove(kobject_t *child);

/// @brief Iterate over all children of a parent kobject.
/// @param[in] parent The parent kobject.
/// @param[in] fn     Callback function for each child.
/// @param[in] data   User data for the callback.
/// @warning fn must not modify the hierarchy of the parent (e.g. no add/remove).
void koms_children_walk(kobject_t *parent,
                        void (*fn)(kobject_t *child, void *data),
                        void *data);

/// @brief Dump a single kobject summary to the kernel log.
/// @param[in] obj The kobject to dump.
void koms_dump_obj(const kobject_t *obj);

/// @brief Recursively dump a kobject tree to the kernel log.
/// @param[in] obj   Root of the tree to dump.
/// @param[in] depth Initial indentation depth.
void koms_dump_tree(const kobject_t *obj, uint32_t depth);

/// @brief Dump all objects in a namespace to the kernel log.
/// @param[in] ns The namespace to dump.
void koms_dump_ns(const kobj_ns_t *ns);
