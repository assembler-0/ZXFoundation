// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/object/koms.h
//
/// @brief Kernel Object Management System (KOMS).

#pragma once

#include <zxfoundation/object/kref.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sync/rcu.h>
#include <zxfoundation/memory/kmalloc.h>
#include <zxfoundation/memory/slab.h>
#include <lib/list.h>

typedef struct kobj_type kobj_type_t;
typedef struct kobj_ns kobj_ns_t;
typedef struct kobj_attr kobj_attr_t;
typedef struct kobj_event kobj_event_t;
typedef struct kobj_listener kobj_listener_t;
typedef struct kobject kobject_t;

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

/// Object was allocated by koms_alloc() and must be freed by koms_free().
#define KOBJ_FLAG_KOMS_ALLOC    (1U << 0)
/// Object is registered in a namespace.
#define KOBJ_FLAG_IN_NS         (1U << 1)
/// Object has at least one attribute attached.
#define KOBJ_FLAG_HAS_ATTRS     (1U << 2)
/// Object has at least one event listener attached.
#define KOBJ_FLAG_HAS_LISTENERS (1U << 3)
/// Object is frozen: no new references may be acquired.
#define KOBJ_FLAG_FROZEN        (1U << 4)

typedef enum {
    KOBJECT_UNINITIALIZED = 0,
    KOBJECT_ALIVE,
    KOBJECT_DEAD,
} kobject_state_t;

typedef enum {
    KOBJ_EVENT_NONE = 0,
    KOBJ_EVENT_CREATED,
    KOBJ_EVENT_DESTROYED,
    KOBJ_EVENT_STATE_CHANGE,
    KOBJ_EVENT_ATTR_SET,
    KOBJ_EVENT_CHILD_ADD,
    KOBJ_EVENT_CHILD_REMOVE,
    KOBJ_EVENT_USER_BASE = 256,
} kobj_event_type_t;

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

struct kobj_event {
    kobj_event_type_t type;
    kobject_t *source;
    kobj_event_payload_t payload;
};

typedef void (*kobj_event_fn_t)(const kobj_event_t *event, void *data);

struct kobj_listener {
    list_node_t node;
    kobj_event_fn_t fn;
    void *data;
    /// Bitmask of kobj_event_type_t values to receive; 0 = all events.
    uint32_t event_mask;
};

/// @param buf   Output buffer.
/// @param size  Buffer capacity.
/// @return Bytes written, or negative errno.
typedef int (*kobj_attr_get_fn_t)(kobject_t *obj, const kobj_attr_t *attr,
                                  char *buf, size_t size);

/// @param buf   Input string.
/// @param size  Input length (excluding NUL).
/// @return Bytes consumed, or negative errno.
typedef int (*kobj_attr_set_fn_t)(kobject_t *obj, const kobj_attr_t *attr,
                                  const char *buf, size_t size);

struct kobj_attr {
    list_node_t node;
    const char *name;
    kobj_attr_get_fn_t get; ///< May be nullptr (write-only attribute).
    kobj_attr_set_fn_t set; ///< May be nullptr (read-only attribute).
};

typedef struct kobject_ops {
    /// @brief Called when the last reference is dropped.
    ///        Must free the containing object.  No locks held.
    void (*release)(kobject_t *obj);
} kobject_ops_t;

typedef struct {
    /// Called after koms_alloc() zeroes the object.  May be nullptr.
    void (*init)(kobject_t *obj);

    /// Called just before ops->release().  May be nullptr.
    void (*destroy)(kobject_t *obj);

    /// Called when the object is added to a namespace.  May be nullptr.
    void (*ns_add)(kobject_t *obj, kobj_ns_t *ns);

    /// Called when the object is removed from a namespace.  May be nullptr.
    void (*ns_remove)(kobject_t *obj, kobj_ns_t *ns);
} kobj_type_ops_t;

struct kobj_type {
    uint32_t type_id;
    const char *name;
    size_t obj_size; ///< sizeof(containing struct).
    kmem_cache_t *cache; ///< nullptr → kmalloc fallback.
    const kobject_ops_t *kobj_ops; ///< Mandatory: must provide release().
    const kobj_type_ops_t *type_ops; ///< May be nullptr.
    list_node_t registry_node;
};

#define KOBJ_NS_BUCKETS 64U

typedef struct {
    list_node_t chain;
} kobj_ns_bucket_t;

struct kobj_ns {
    kobject_t *owner;
    kobj_ns_t *parent;
    list_node_t children;
    list_node_t sibling;
    spinlock_t write_lock;
    kobj_ns_bucket_t buckets[KOBJ_NS_BUCKETS];
    uint32_t count;
    const char *name;
};

struct kobject {
    kref_t ref;
    const kobject_ops_t *ops;
    kobject_state_t state;
    const char *name;

    // KOMS extensions
    uint32_t type_id;
    uint32_t flags;
    kobject_t *parent;
    list_node_t sibling;
    list_node_t children;
    list_node_t attrs;
    list_node_t listeners;
    rcu_head_t rcu;
    spinlock_t lock;
    kobj_ns_t *ns;
    list_node_t ns_node;
};

/// @brief Recover the containing struct from an embedded kobject pointer.
#define kobject_container(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

extern kobj_ns_t koms_root_ns;

/// @brief Initialize KOMS.  Call after kmalloc_init(), before any subsystem
///        that uses koms_type_register() or koms_alloc().
void koms_init(void);

/// @brief Register a type descriptor.  type->type_id must be unique.
void koms_type_register(kobj_type_t *type);

/// @brief Look up a type by ID.  Lockless; safe from any context.
/// @return Pointer to the type, or nullptr if not found.
const kobj_type_t *koms_type_lookup(uint32_t type_id);

/// @brief Allocate and zero an object of the given type.
///        Returns with refcount = 0; call koms_init_obj() next.
/// @return Pointer to the embedded kobject_t, or nullptr on OOM.
kobject_t *koms_alloc(const kobj_type_t *type, gfp_t gfp);

/// @brief Initialize a kobject (allocated or statically embedded).
///        Sets refcount = 1, wires ops/type/name/parent.
///        Calls type_ops->init() if present.
///        Fires KOBJ_EVENT_CREATED.
/// @param type    May be nullptr → KOBJ_TYPE_GENERIC with no slab.
/// @param parent  May be nullptr.
void koms_init_obj(kobject_t *obj, const kobj_type_t *type,
                   const char *name, kobject_t *parent);

/// @brief Increment the reference count.
/// @return obj (for chaining).
kobject_t *koms_get(kobject_t *obj);

/// @brief Decrement the reference count.
///        At zero: fires KOBJ_EVENT_DESTROYED, calls type_ops->destroy(),
///        then calls ops->release().
void koms_put(kobject_t *obj);

/// @brief Increment only if the object is not frozen or dead.
///        Safe under RCU.
bool koms_get_unless_dead(kobject_t *obj);

/// @brief Prevent new references from being acquired.
///        Existing references remain valid.
void koms_freeze(kobject_t *obj);

/// @brief Free an object allocated by koms_alloc().
///        Must only be called from ops->release().
void koms_free(kobject_t *obj);

/// @brief Initialize a namespace.
/// @param parent  nullptr → attach to koms_root_ns.
void koms_ns_init(kobj_ns_t *ns, const char *name,
                  kobj_ns_t *parent, kobject_t *owner);

/// @brief Register obj in ns under obj->name.
/// @return 0 on success, -1 if name already exists.
int koms_ns_add(kobj_ns_t *ns, kobject_t *obj);

/// @brief Remove obj from its namespace.
void koms_ns_remove(kobject_t *obj);

/// @brief Look up by name (RCU read-side, no refcount bump).
///        Must be called inside rcu_read_lock().
kobject_t *koms_ns_find(kobj_ns_t *ns, const char *name);

/// @brief Look up by name and acquire a reference.
kobject_t *koms_ns_find_get(kobj_ns_t *ns, const char *name);

/// @brief Attach an attribute.
/// @return 0 on success, -1 if name already exists.
int koms_attr_add(kobject_t *obj, kobj_attr_t *attr);

/// @brief Detach an attribute.
void koms_attr_remove(kobject_t *obj, kobj_attr_t *attr);

/// @brief Read an attribute value.
/// @return Bytes written, or -1 if not found.
int koms_attr_get(kobject_t *obj, const char *name, char *buf, size_t size);

/// @brief Write an attribute value.  Fires KOBJ_EVENT_ATTR_SET on success.
/// @return Bytes consumed, or -1 if not found or read-only.
int koms_attr_set(kobject_t *obj, const char *name, const char *buf, size_t size);

/// @brief Register an event listener on obj.
void koms_listener_add(kobject_t *obj, kobj_listener_t *listener);

/// @brief Unregister an event listener.
void koms_listener_remove(kobject_t *obj, kobj_listener_t *listener);

/// @brief Dispatch an event to all listeners on obj, then propagate to parent.
void koms_event_dispatch(kobject_t *obj, kobj_event_t *event);

/// @brief Build and dispatch a simple typed event.
static inline void koms_event_fire(kobject_t *obj, kobj_event_type_t type) {
    kobj_event_t ev = {.type = type, .source = obj};
    koms_event_dispatch(obj, &ev);
}

/// @brief Add child as a child of parent.  Fires KOBJ_EVENT_CHILD_ADD.
void koms_child_add(kobject_t *parent, kobject_t *child);

/// @brief Remove child from its parent.  Fires KOBJ_EVENT_CHILD_REMOVE.
void koms_child_remove(kobject_t *child);

/// @brief Walk the child list, calling fn for each child.
///        fn must not call koms_child_add/remove on parent.
void koms_children_walk(kobject_t *parent,
                        void (*fn)(kobject_t *child, void *data),
                        void *data);

/// @brief Print a one-line summary of obj to the kernel log.
void koms_dump_obj(const kobject_t *obj);

/// @brief Print the full subtree rooted at obj.
void koms_dump_tree(const kobject_t *obj, uint32_t depth);

/// @brief Print all objects registered in a namespace.
void koms_dump_ns(const kobj_ns_t *ns);
