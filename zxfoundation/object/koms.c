/// SPDX-License-Identifier: Apache-2.0
/// @file koms.c
/// @brief Kernel Object Management System — full implementation.

#include <zxfoundation/object/koms.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/common.h>
#include <lib/string.h>

/// @brief Global registry of all kobject types.
static list_head_t type_registry;
/// @brief Protects access to the type_registry.
static spinlock_t type_registry_lock;

/// @brief Register a new kobject type.
/// @param[in,out] type The type descriptor to register.
/// @note Checks for duplicate type_ids.
void koms_type_register(kobj_type_t *type) {
    irqflags_t flags;
    spin_lock_irqsave(&type_registry_lock, &flags);

    kobj_type_t *t;
    list_for_each_entry(t, &type_registry, registry_node) {
        if (t->type_id == type->type_id) {
            spin_unlock_irqrestore(&type_registry_lock, flags);
            printk(ZX_ERROR "koms: duplicate type_id %u (%s)\n",
                   type->type_id, type->name);
            return;
        }
    }

    list_init(&type->registry_node);
    list_add_tail(&type->registry_node, &type_registry);
    spin_unlock_irqrestore(&type_registry_lock, flags);
}

/// @brief Look up a type descriptor by its ID.
/// @param[in] type_id The ID to search for.
/// @return Pointer to the descriptor, or nullptr if not found.
const kobj_type_t *koms_type_lookup(uint32_t type_id) {
    // Note: This walk is currently unprotected by a lock.
    // In a dynamic registry, this would need RCU or a lock.
    kobj_type_t *t;
    list_for_each_entry(t, &type_registry, registry_node) {
        if (t->type_id == type_id)
            return t;
    }
    return nullptr;
}

/// @brief The root KOMS namespace.
kobj_ns_t koms_root_ns;

/// @section object initialization

/// @brief Initialize the KOMS subsystem.
/// @note Sets up the type registry and the root namespace.
void koms_init(void) {
    list_init(&type_registry);
    spin_lock_init(&type_registry_lock);
    koms_ns_init(&koms_root_ns, "root", nullptr, nullptr);
    printk(ZX_INFO "koms: initialized\n");
}

/// @brief Initialize a kobject.
/// @param[in,out] obj    The kobject to initialize.
/// @param[in]     type   Type descriptor.
/// @param[in]     name   Object name.
/// @param[in,out] parent Parent kobject.
void koms_init_obj(kobject_t *obj, const kobj_type_t *type,
                   const char *name, kobject_t *parent) {
    kref_init(&obj->ref);
    obj->ops = type ? type->kobj_ops : nullptr;
    obj->state = KOBJECT_ALIVE;
    obj->name = name;
    obj->type_id = type ? type->type_id : KOBJ_TYPE_GENERIC;
    obj->parent = nullptr;
    obj->ns = nullptr;

    list_init(&obj->sibling);
    list_init(&obj->children);
    list_init(&obj->attrs);
    list_init(&obj->listeners);
    list_init(&obj->ns_node);
    spin_lock_init(&obj->lock);

    if (type && type->type_ops && type->type_ops->init)
        type->type_ops->init(obj);

    if (parent)
        koms_child_add(parent, obj);

    koms_event_fire(obj, KOBJ_EVENT_CREATED);
}

/// @section object allocation

/// @brief Allocate a kobject.
/// @param[in] type Type descriptor.
/// @param[in] gfp  Allocation flags.
/// @return Pointer to the kobject, or nullptr on failure.
kobject_t *koms_alloc(const kobj_type_t *type, gfp_t gfp) {
    void *raw;
    size_t sz = type ? type->obj_size : sizeof(kobject_t);

    if (type && type->cache)
        raw = kmem_cache_alloc(type->cache, gfp | ZX_GFP_ZERO);
    else
        raw = kzalloc(sz, gfp);

    if (unlikely(!raw))
        return nullptr;

    kobject_t *obj = (kobject_t *) raw;
    obj->flags |= KOBJ_FLAG_KOMS_ALLOC;
    return obj;
}

/// @brief Free a kobject allocated via koms_alloc().
/// @param[in,out] obj The kobject to free.
void koms_free(kobject_t *obj) {
    if (unlikely(!obj))
        return;
    const kobj_type_t *type = koms_type_lookup(obj->type_id);
    if ((obj->flags & KOBJ_FLAG_KOMS_ALLOC) && type && type->cache)
        kmem_cache_free(type->cache, obj);
    else
        kfree(obj);
}

/// @section reference counting

/// @brief Acquire a reference to a kobject.
/// @param[in,out] obj The kobject.
/// @return The same kobject pointer.
kobject_t *koms_get(kobject_t *obj) {
    if (likely(obj))
        kref_get(&obj->ref);
    return obj;
}

/// @brief Internal release handler called when refcount hits zero.
/// @param[in,out] ref Pointer to the embedded kref_t.
static void koms_release_internal(kref_t *ref) {
    kobject_t *obj = kobject_container(ref, kobject_t, ref);

    obj->state = KOBJECT_DEAD;

    koms_event_fire(obj, KOBJ_EVENT_DESTROYED);

    // Auto-cleanup from containers.
    if (obj->flags & KOBJ_FLAG_IN_NS)
        koms_ns_remove(obj);

    if (obj->parent)
        koms_child_remove(obj);

    const kobj_type_t *type = koms_type_lookup(obj->type_id);
    if (type && type->type_ops && type->type_ops->destroy)
        type->type_ops->destroy(obj);

    // Final release: frees the memory.
    if (likely(obj->ops && obj->ops->release))
        obj->ops->release(obj);
}

/// @brief Release a reference to a kobject.
/// @param[in,out] obj The kobject.
void koms_put(kobject_t *obj) {
    if (likely(obj))
        kref_put(&obj->ref, koms_release_internal);
}

/// @brief Acquire a reference if the object is still alive.
/// @param[in,out] obj The kobject.
/// @return true if reference was acquired, false otherwise.
bool koms_get_unless_dead(kobject_t *obj) {
    if (unlikely(!obj))
        return false;
    if (obj->state != KOBJECT_ALIVE)
        return false;
    if (obj->flags & KOBJ_FLAG_FROZEN)
        return false;
    return kref_get_unless_zero(&obj->ref);
}

/// @brief Freeze an object.
/// @param[in,out] obj The kobject.
void koms_freeze(kobject_t *obj) {
    irqflags_t flags;
    spin_lock_irqsave(&obj->lock, &flags);
    obj->flags |= KOBJ_FLAG_FROZEN;
    spin_unlock_irqrestore(&obj->lock, flags);
}

/// @section namespace management

/// @brief Simple hash function for namespace strings.
static inline uint32_t ns_hash(const char *name) {
    uint32_t hash = 5381;
    int c;
    while ((c = *name++))
        hash = ((hash << 5) + hash) + (uint32_t)c;
    return hash % KOBJ_NS_BUCKETS;
}

/// @brief Initialize a kobject namespace.
/// @param[in,out] ns     The namespace.
/// @param[in]     name   Name of the namespace.
/// @param[in,out] parent Parent namespace.
/// @param[in,out] owner  Object owning this namespace.
void koms_ns_init(kobj_ns_t *ns, const char *name,
                  kobj_ns_t *parent, kobject_t *owner) {
    ns->name = name;
    ns->owner = owner;
    ns->count = 0;
    ns->parent = parent ? parent : (ns == &koms_root_ns ? nullptr : &koms_root_ns);
    spin_lock_init(&ns->write_lock);
    list_init(&ns->children);
    list_init(&ns->sibling);

    for (uint32_t i = 0; i < KOBJ_NS_BUCKETS; i++)
        list_init(&ns->buckets[i].chain);

    if (ns->parent) {
        irqflags_t flags;
        spin_lock_irqsave(&ns->parent->write_lock, &flags);
        list_add_tail(&ns->sibling, &ns->parent->children);
        spin_unlock_irqrestore(&ns->parent->write_lock, flags);
    }
}

/// @brief Add an object to a namespace.
/// @param[in,out] ns  The namespace.
/// @param[in,out] obj The kobject.
/// @return 0 on success, -1 if duplicate name.
int koms_ns_add(kobj_ns_t *ns, kobject_t *obj) {
    if (unlikely(!ns || !obj || !obj->name))
        return -1;

    uint32_t bucket = ns_hash(obj->name);
    irqflags_t flags;
    spin_lock_irqsave(&ns->write_lock, &flags);

    kobject_t *existing;
    list_for_each_entry(existing, &ns->buckets[bucket].chain, ns_node) {
        if (strcmp(existing->name, obj->name) == 0) {
            spin_unlock_irqrestore(&ns->write_lock, flags);
            return -1;
        }
    }

    list_init(&obj->ns_node);
    list_add_tail(&obj->ns_node, &ns->buckets[bucket].chain);
    obj->ns = ns;
    obj->flags |= KOBJ_FLAG_IN_NS;
    ns->count++;
    spin_unlock_irqrestore(&ns->write_lock, flags);

    const kobj_type_t *type = koms_type_lookup(obj->type_id);
    if (type && type->type_ops && type->type_ops->ns_add)
        type->type_ops->ns_add(obj, ns);

    return 0;
}

/// @brief Remove an object from its namespace.
/// @param[in,out] obj The kobject.
void koms_ns_remove(kobject_t *obj) {
    if (unlikely(!obj || !(obj->flags & KOBJ_FLAG_IN_NS)))
        return;

    kobj_ns_t *ns = obj->ns;
    if (unlikely(!ns))
        return;

    irqflags_t flags;
    spin_lock_irqsave(&ns->write_lock, &flags);
    list_del(&obj->ns_node);
    list_init(&obj->ns_node);
    obj->flags &= ~KOBJ_FLAG_IN_NS;
    obj->ns = nullptr;
    ns->count--;
    spin_unlock_irqrestore(&ns->write_lock, flags);

    const kobj_type_t *type = koms_type_lookup(obj->type_id);
    if (type && type->type_ops && type->type_ops->ns_remove)
        type->type_ops->ns_remove(obj, ns);
}

/// @brief Look up an object in a namespace.
/// @param[in] ns   The namespace.
/// @param[in] name The name to find.
/// @return Pointer to the kobject, or nullptr if not found.
kobject_t *koms_ns_find(kobj_ns_t *ns, const char *name) {
    if (unlikely(!ns || !name))
        return nullptr;

    uint32_t bucket = ns_hash(name);
    kobject_t *obj;
    list_for_each_entry(obj, &ns->buckets[bucket].chain, ns_node) {
        if (strcmp(obj->name, name) == 0)
            return obj;
    }
    return nullptr;
}

/// @brief Find an object and acquire a reference.
/// @param[in] ns   The namespace.
/// @param[in] name The name to find.
/// @return Pointer to the kobject with reference acquired, or nullptr.
kobject_t *koms_ns_find_get(kobj_ns_t *ns, const char *name) {
    rcu_read_lock();
    kobject_t *obj = koms_ns_find(ns, name);
    if (obj && !koms_get_unless_dead(obj))
        obj = nullptr;
    rcu_read_unlock();
    return obj;
}

/// @section attributes

/// @brief Attach an attribute to an object.
/// @param[in,out] obj  The kobject.
/// @param[in,out] attr The attribute.
/// @return 0 on success, -1 if attribute name exists.
int koms_attr_add(kobject_t *obj, kobj_attr_t *attr) {
    irqflags_t flags;
    spin_lock_irqsave(&obj->lock, &flags);

    kobj_attr_t *a;
    list_for_each_entry(a, &obj->attrs, node) {
        if (strcmp(a->name, attr->name) == 0) {
            spin_unlock_irqrestore(&obj->lock, flags);
            return -1;
        }
    }

    list_init(&attr->node);
    list_add_tail(&attr->node, &obj->attrs);
    obj->flags |= KOBJ_FLAG_HAS_ATTRS;
    spin_unlock_irqrestore(&obj->lock, flags);
    return 0;
}

/// @brief Remove an attribute from an object.
/// @param[in,out] obj  The kobject.
/// @param[in,out] attr The attribute.
void koms_attr_remove(kobject_t *obj, kobj_attr_t *attr) {
    irqflags_t flags;
    spin_lock_irqsave(&obj->lock, &flags);
    list_del(&attr->node);
    list_init(&attr->node);
    spin_unlock_irqrestore(&obj->lock, flags);
}

/// @brief Read an attribute value.
/// @param[in,out] obj  The kobject.
/// @param[in]     name Attribute name.
/// @param[out]    buf  Output buffer.
/// @param[in]     size Buffer capacity.
/// @return Result from the getter, or -1 on failure.
int koms_attr_get(kobject_t *obj, const char *name, char *buf, size_t size) {
    irqflags_t flags;
    spin_lock_irqsave(&obj->lock, &flags);

    kobj_attr_t *found = nullptr;
    kobj_attr_t *a;
    list_for_each_entry(a, &obj->attrs, node) {
        if (strcmp(a->name, name) == 0) {
            found = a;
            break;
        }
    }
    spin_unlock_irqrestore(&obj->lock, flags);

    if (!found || !found->get)
        return -1;
    return found->get(obj, found, buf, size);
}

/// @brief Write an attribute value.
/// @param[in,out] obj  The kobject.
/// @param[in]     name Attribute name.
/// @param[in]     buf  Input string.
/// @param[in]     size Input length.
/// @return Result from the setter, or -1 on failure.
int koms_attr_set(kobject_t *obj, const char *name, const char *buf, size_t size) {
    irqflags_t flags;
    spin_lock_irqsave(&obj->lock, &flags);

    kobj_attr_t *found = nullptr;
    kobj_attr_t *a;
    list_for_each_entry(a, &obj->attrs, node) {
        if (strcmp(a->name, name) == 0) {
            found = a;
            break;
        }
    }
    spin_unlock_irqrestore(&obj->lock, flags);

    if (!found || !found->set)
        return -1;

    int ret = found->set(obj, found, buf, size);
    if (ret >= 0) {
        kobj_event_t ev = {
            .type = KOBJ_EVENT_ATTR_SET,
            .source = obj,
            .payload = {.attr_set = {.attr = found}},
        };
        koms_event_dispatch(obj, &ev);
    }
    return ret;
}

/// @section events

/// @brief Add an event listener to an object.
/// @param[in,out] obj      The kobject.
/// @param[in,out] listener The listener.
void koms_listener_add(kobject_t *obj, kobj_listener_t *listener) {
    irqflags_t flags;
    spin_lock_irqsave(&obj->lock, &flags);
    list_init(&listener->node);
    list_add_tail(&listener->node, &obj->listeners);
    obj->flags |= KOBJ_FLAG_HAS_LISTENERS;
    spin_unlock_irqrestore(&obj->lock, flags);
}

/// @brief Remove an event listener from an object.
/// @param[in,out] obj      The kobject.
/// @param[in,out] listener The listener to remove.
void koms_listener_remove(kobject_t *obj, kobj_listener_t *listener) {
    irqflags_t flags;
    spin_lock_irqsave(&obj->lock, &flags);
    list_del(&listener->node);
    list_init(&listener->node);
    spin_unlock_irqrestore(&obj->lock, flags);
}

/// @brief Dispatch an event.
/// @param[in,out] obj   Source kobject.
/// @param[in,out] event The event.
void koms_event_dispatch(kobject_t *obj, kobj_event_t *event) {
    if (unlikely(!obj))
        return;

    if (likely(obj->flags & KOBJ_FLAG_HAS_LISTENERS)) {
#define KOMS_LISTENER_SNAP 16U
        kobj_listener_t *snap[KOMS_LISTENER_SNAP];
        uint32_t n = 0;

        irqflags_t flags;
        spin_lock_irqsave(&obj->lock, &flags);
        kobj_listener_t *l;
        list_for_each_entry(l, &obj->listeners, node) {
            if (n >= KOMS_LISTENER_SNAP) break;
            snap[n++] = l;
        }
        spin_unlock_irqrestore(&obj->lock, flags);

        for (uint32_t i = 0; i < n; i++) {
            kobj_listener_t *sl = snap[i];
            if (!sl->event_mask || (sl->event_mask & (1U << (uint32_t) event->type)))
                sl->fn(event, sl->data);
        }
#undef KOMS_LISTENER_SNAP
    }

    if (obj->parent)
        koms_event_dispatch(obj->parent, event);
}

/// @section hierarchy

/// @brief Add a child to a parent object.
/// @param[in,out] parent The parent kobject.
/// @param[in,out] child  The child kobject.
void koms_child_add(kobject_t *parent, kobject_t *child) {
    irqflags_t flags;
    spin_lock_irqsave(&parent->lock, &flags);
    list_init(&child->sibling);
    list_add_tail(&child->sibling, &parent->children);
    child->parent = parent;
    spin_unlock_irqrestore(&parent->lock, flags);

    kobj_event_t ev = {
        .type = KOBJ_EVENT_CHILD_ADD,
        .source = parent,
        .payload = {.child = {.child = child}},
    };
    koms_event_dispatch(parent, &ev);
}

/// @brief Remove a child from its parent.
/// @param[in,out] child The child kobject.
void koms_child_remove(kobject_t *child) {
    kobject_t *parent = child->parent;
    if (unlikely(!parent))
        return;

    irqflags_t flags;
    spin_lock_irqsave(&parent->lock, &flags);
    list_del(&child->sibling);
    list_init(&child->sibling);
    child->parent = nullptr;
    spin_unlock_irqrestore(&parent->lock, flags);

    kobj_event_t ev = {
        .type = KOBJ_EVENT_CHILD_REMOVE,
        .source = parent,
        .payload = {.child = {.child = child}},
    };
    koms_event_dispatch(parent, &ev);
}

/// @brief Iterate over all children of an object.
/// @param[in] parent The parent kobject.
/// @param[in] fn     Callback function.
/// @param[in] data   User data.
void koms_children_walk(kobject_t *parent,
                        void (*fn)(kobject_t *child, void *data),
                        void *data) {
    irqflags_t flags;
    spin_lock_irqsave(&parent->lock, &flags);
    kobject_t *child;
    list_for_each_entry(child, &parent->children, sibling) {
        fn(child, data);
    }
    spin_unlock_irqrestore(&parent->lock, flags);
}

/// @section diagnostics

/// @brief Convert object state to string.
static const char *state_str(kobject_state_t s) {
    switch (s) {
        case KOBJECT_UNINITIALIZED: return "uninitialized";
        case KOBJECT_ALIVE: return "alive";
        case KOBJECT_DEAD: return "dead";
        default: return "unknown";
    }
}

/// @brief Dump object summary to log.
/// @param[in] obj The kobject.
void koms_dump_obj(const kobject_t *obj) {
    if (!obj) {
        printk("koms: (null)\n");
        return;
    }
    printk(ZX_INFO "koms: [%s] type=%u state=%s flags=0x%x refs=%d ns=%s\n",
           obj->name ? obj->name : "(unnamed)",
           obj->type_id,
           state_str(obj->state),
           obj->flags,
           atomic_read((atomic_t *) &obj->ref.refcount),
           obj->ns ? obj->ns->name : "(none)");
}

/// @brief Recursively dump object tree.
/// @param[in] obj   The root kobject.
/// @param[in] depth Initial depth.
void koms_dump_tree(const kobject_t *obj, uint32_t depth) {
    if (!obj) return;
    for (uint32_t i = 0; i < depth; i++) printk("  ");
    koms_dump_obj(obj);
    kobject_t *child;
    list_for_each_entry(child, (list_head_t *)&obj->children, sibling) {
        koms_dump_tree(child, depth + 1);
    }
}

/// @brief Dump all objects in a namespace.
/// @param[in] ns The namespace.
void koms_dump_ns(const kobj_ns_t *ns) {
    if (!ns) return;
    printk(ZX_INFO "koms: namespace '%s' (%u objects)\n", ns->name, ns->count);
    for (uint32_t i = 0; i < KOBJ_NS_BUCKETS; i++) {
        kobject_t *obj;
        list_for_each_entry(obj, (list_head_t *)&ns->buckets[i].chain, ns_node) {
            printk(ZX_INFO "  [%u] ", i);
            koms_dump_obj(obj);
        }
    }
}
