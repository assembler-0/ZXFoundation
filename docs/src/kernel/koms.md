# Kernel Object Management System

**Document:** ZXF-KRN-KOMS-001  
**Revision:** 1.0  
**Status:** Released

---

## 1. Purpose

The Kernel Object Management System (KOMS) is the unified abstraction layer
for all reference-counted kernel objects.  It defines a single base type,
`kobject_t`, that any subsystem may embed to obtain lifecycle management,
naming, attribute storage, event delivery, and hierarchical organization at
no additional per-subsystem cost.

---

## 2. Architectural Position

KOMS sits immediately above the memory allocator and synchronization
primitives, and below all subsystems that manage named, reference-counted
resources.

```
┌─────────────────────────────────────────────────────┐
│  Subsystems  (IRQ, VMM, Device, Task, File, …)      │
├─────────────────────────────────────────────────────┤
│  KOMS  (koms.h / koms.c)                            │
├──────────────┬──────────────┬───────────────────────┤
│  kmalloc /   │  spinlock /  │  RCU                  │
│  slab        │  rwlock      │                       │
└──────────────┴──────────────┴───────────────────────┘
```

KOMS is initialized once, after `kmalloc_init()`, before any subsystem that
registers a type or allocates a managed object.

---

## 3. Core Concepts

### 3.1 kobject_t

Every managed object embeds `kobject_t` as its first member.  The base
object carries:

- An atomic reference counter (`kref_t`).
- A mandatory operations table (`kobject_ops_t`) with a `release` callback.
- A lifecycle state (`KOBJECT_UNINITIALIZED`, `KOBJECT_ALIVE`, `KOBJECT_DEAD`).
- A static name string.
- A 32-bit type identifier.
- A 32-bit flags word.
- Intrusive list nodes for parent/child hierarchy, namespace membership,
  attributes, and event listeners.
- An embedded `spinlock_t` protecting the mutable extension fields.
- An `rcu_head_t` for deferred free.

The `kobject_container()` macro recovers the containing struct from a
`kobject_t *` pointer using compile-time offset arithmetic.

### 3.2 Type Registry

A `kobj_type_t` descriptor is registered once at boot per object class.
It carries:

| Field | Purpose |
|---|---|
| `type_id` | Globally unique 32-bit identifier |
| `name` | Human-readable string for diagnostics |
| `obj_size` | `sizeof` of the containing struct |
| `cache` | Optional dedicated slab cache |
| `kobj_ops` | Mandatory ops table (must provide `release`) |
| `type_ops` | Optional extended vtable (`init`, `destroy`, `ns_add`, `ns_remove`) |

After `koms_init()` the registry is append-only and read locklessly.

### 3.3 Namespace

A `kobj_ns_t` is an RCU-protected hash table of `kobject_t` pointers,
keyed by name.  Namespaces form a tree rooted at `koms_root_ns`.

```
koms_root_ns
├── "irq"
│   ├── "ext-0x40"
│   └── "pgm-0x0d"
├── "vmm"
│   └── "kernel"
└── "device"
    └── "dasd-0"
```

Reads use `rcu_read_lock()` and are fully lockless.  Writes acquire the
namespace's `write_lock` (spinlock, irqsave).

### 3.4 Attributes

Attributes are `kobj_attr_t` nodes linked into `kobject_t::attrs`.  Each
attribute has a name and optional `get`/`set` callbacks.  The attribute list
is protected by `kobject_t::lock`.

### 3.5 Event Bus

Events are typed (`kobj_event_type_t`) and carry a payload union.
Listeners (`kobj_listener_t`) are registered per-object with an optional
event-type bitmask filter.  Dispatch snapshots the listener list under the
object lock, then calls each listener without the lock, preventing deadlocks
on re-entrant dispatch.  Events propagate up the parent chain automatically.

---

## 4. Lifecycle

```
         koms_alloc()
              │
              ▼
        [refcount = 0]
              │
        koms_init_obj()
              │
              ▼
        KOBJECT_ALIVE  ◄──── koms_get()
        [refcount = 1]
              │
        koms_put() × N
              │
        [refcount = 0]
              │
              ▼
         KOBJECT_DEAD
              │
         ops->release()
              │
              ▼
          koms_free()
```

`koms_freeze()` sets `KOBJ_FLAG_FROZEN`, causing `koms_get_unless_dead()` to
fail without affecting existing references.  This enables controlled
teardown: freeze the object, wait for all external references to drain, then
drop the final reference.

---

## 5. Allocation Strategy

```
koms_alloc(type, gfp)
    │
    ├─ type->cache != nullptr ──► kmem_cache_alloc(type->cache, gfp | ZERO)
    │
    └─ type->cache == nullptr ──► kzalloc(type->obj_size, gfp)
```

`koms_free()` dispatches symmetrically.  The `KOBJ_FLAG_KOMS_ALLOC` flag
distinguishes heap-allocated objects from statically embedded ones.

---

## 6. Thread Safety Summary

| Operation | Mechanism |
|---|---|
| Reference count | Lock-free (CS instruction) |
| Attribute list | `kobject_t::lock` (spinlock, irqsave) |
| Listener list | `kobject_t::lock` (spinlock, irqsave) |
| Child list | `kobject_t::lock` (spinlock, irqsave) |
| Namespace reads | `rcu_read_lock()` (lockless) |
| Namespace writes | `kobj_ns_t::write_lock` (spinlock, irqsave) |
| Type registry reads | Lockless (append-only after boot) |
| Type registry writes | `type_registry_lock` (spinlock, irqsave) |

---

## 7. Integration Guide

To integrate a subsystem with KOMS:

1. Embed `kobject_t` as the **first member** of the subsystem struct.
2. Define a `kobject_ops_t` with a `release` callback that calls `koms_free()`.
3. Optionally define a `kobj_type_ops_t` for `init`/`destroy` hooks.
4. Define and register a `kobj_type_t` from the subsystem's init function.
5. Allocate objects with `koms_alloc()` and initialize with `koms_init_obj()`.
6. Use `koms_get()` / `koms_put()` for reference management.
7. Optionally register in a namespace with `koms_ns_add()`.

---

## 8. Initialization Order

KOMS must be initialized after `kmalloc_init()` and before any subsystem
that calls `koms_type_register()` or `koms_alloc()`.

```
pmm_init → cma_init → mmu_init → vmm_init → slab_init → kmalloc_init
    → koms_init → smp_init → [subsystem inits]
```
