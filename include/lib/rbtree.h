// SPDX-License-Identifier: Apache-2.0
// include/lib/rbtree.h
//
/// @brief Intrusive red-black tree — freestanding, SMP-safe, with augmented
///        gap-tree, RCU-augmented composite, and per-CPU cached variants.
///
///        LAYER OVERVIEW
///        ==============
///        Layer 0  rb_root_t          Plain tree, no concurrency.
///        Layer 1  rb_root_aug_t      Augmented (propagate callback).
///        Layer 2  rcu_rb_root_t      RCU readers + write spinlock.
///        Layer 2A rcu_rb_root_aug_t  RCU + augmented (unified write lock).
///        Layer 3  rb_pcpu_cache_t    Per-CPU O(1) hint cache.
///        Layer 4  vmm_rb_root_t      Layers 2A + 3 for the VMA tree.
///
///        NODE LAYOUT
///        ===========
///        rb_node_t     — 24 bytes: left(8) + right(8) + parent_and_color(8).
///        rb_node_aug_t — 32 bytes: rb_node_t(24) + subtree_max_end(8).
///
///        The color bit occupies bit 0 of parent_and_color.  All rb_node_t
///        pointers are 8-byte aligned on s390x, so bit 0 is always free.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sync/rcu.h>

// ---------------------------------------------------------------------------
// Layer 0 — Core node and root
// ---------------------------------------------------------------------------

typedef struct rb_node {
    struct rb_node *left;
    struct rb_node *right;
    uintptr_t       parent_and_color;  ///< parent ptr | colour (bit 0)
} rb_node_t;

_Static_assert(sizeof(rb_node_t) == 24, "rb_node_t must be 24 bytes");

typedef struct {
    rb_node_t *root;
} rb_root_t;

#define RB_ROOT_INIT    { .root = nullptr }

#define RB_RED    0U
#define RB_BLACK  1U

static inline rb_node_t *rb_parent(const rb_node_t *n) {
    return (rb_node_t *)(n->parent_and_color & ~(uintptr_t)1);
}
static inline unsigned rb_color(const rb_node_t *n) {
    return (unsigned)(n->parent_and_color & 1U);
}
static inline bool rb_is_red  (const rb_node_t *n) { return rb_color(n) == RB_RED;   }
static inline bool rb_is_black(const rb_node_t *n) { return rb_color(n) == RB_BLACK; }
static inline void rb_set_parent(rb_node_t *n, rb_node_t *p) {
    n->parent_and_color = (n->parent_and_color & 1U) | (uintptr_t)p;
}
static inline void rb_set_color(rb_node_t *n, unsigned c) {
    n->parent_and_color = (n->parent_and_color & ~(uintptr_t)1) | c;
}
static inline void rb_set_parent_color(rb_node_t *n, rb_node_t *p, unsigned c) {
    n->parent_and_color = (uintptr_t)p | c;
}

/// @brief Recover the enclosing struct from an embedded rb_node_t pointer.
#define rb_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

/// @brief Null-safe rb_entry.
#define rb_entry_safe(ptr, type, member) \
    ({ typeof(ptr) _p = (ptr); _p ? rb_entry(_p, type, member) : nullptr; })

rb_node_t *rb_first(const rb_root_t *tree);
rb_node_t *rb_last (const rb_root_t *tree);
rb_node_t *rb_next (const rb_node_t *node);
rb_node_t *rb_prev (const rb_node_t *node);

/// @brief Link a new node at the position found by the caller's tree walk.
/// @param node    New (RED) node.
/// @param parent  Parent node (nullptr if tree is empty).
/// @param link    &parent->left, &parent->right, or &tree->root.
void rb_link_node(rb_node_t *node, rb_node_t *parent, rb_node_t **link);

/// @brief Restore RB invariants after rb_link_node().
void rb_insert_fixup(rb_root_t *tree, rb_node_t *node);

/// @brief Remove a node from the tree, restoring RB invariants.
void rb_erase(rb_root_t *tree, rb_node_t *node);

#define rb_for_each(pos, tree) \
    for ((pos) = rb_first(tree); (pos); (pos) = rb_next(pos))

#define rb_for_each_entry(pos, tree, member) \
    for ((pos) = rb_entry_safe(rb_first(tree), __typeof__(*(pos)), member); \
         (pos); \
         (pos) = rb_entry_safe(rb_next(&(pos)->member), __typeof__(*(pos)), member))

/// @brief Augmented node.  Callers embed this instead of rb_node_t when they
///        need a per-node subtree aggregate.
///
///        subtree_max_end stores the maximum interval endpoint in this subtree.
///        The propagate callback recomputes it in O(1) from the node's own
///        endpoint and its children's subtree_max_end values — no tree traversal.
///        Gap sizes are derived during search descent, not stored directly.
///
///        subtree_max_end must not be written directly by callers.
typedef struct rb_node_aug {
    rb_node_t node;              ///< Must be first — cast-compatible with rb_node_t.
    uint64_t  subtree_max_end;   ///< Max interval end in this subtree.
} rb_node_aug_t;

_Static_assert(sizeof(rb_node_aug_t) == 32, "rb_node_aug_t must be 32 bytes");
_Static_assert(__builtin_offsetof(rb_node_aug_t, node) == 0,
               "rb_node_aug_t::node must be at offset 0");

/// @brief Augmentation callbacks.
///
///        propagate — recompute node's subtree_max_end from its own endpoint
///                    and those of its children.  O(1) — no tree traversal.
///                    Called bottom-up after every structural change.
///
///        copy      — called when the successor physically replaces a deleted
///                    node during rb_erase_aug().  Must copy subtree_max_end
///                    from src to dst.
typedef struct {
    void (*propagate)(rb_node_t *node);
    void (*copy)     (rb_node_t *dst, const rb_node_t *src);
} rb_aug_callbacks_t;

typedef struct {
    rb_root_t              base;
    const rb_aug_callbacks_t *cb;  ///< Must not be nullptr.
} rb_root_aug_t;

#define RB_ROOT_AUG_INIT(callbacks)  { .base = RB_ROOT_INIT, .cb = (callbacks) }

/// @brief Insert into an augmented tree and propagate aggregates.
/// @param root    Augmented root.
/// @param node    New node (caller has initialised its own aggregate field).
/// @param parent  Parent found by caller's tree walk.
/// @param link    &parent->left / &parent->right / &root->base.root.
void rb_insert_aug(rb_root_aug_t *root, rb_node_t *node,
                   rb_node_t *parent, rb_node_t **link);

/// @brief Erase from an augmented tree and propagate aggregates.
void rb_erase_aug(rb_root_aug_t *root, rb_node_t *node);

/// @brief RCU-safe root.
///
///        READER PROTOCOL
///        ───────────────
///        rcu_read_lock();
///        node = rcu_rb_find(&root, cmp, arg);
///        rcu_read_unlock();
///
///        WRITER PROTOCOL
///        ───────────────
///        rcu_rb_insert() / rcu_rb_erase() acquire root->lock internally.
///        Pointer stores use rcu_assign_pointer() so readers see a consistent
///        snapshot.  Erased nodes are freed via call_rcu() after the grace
///        period.
typedef struct {
    rb_root_t   base;
    spinlock_t  lock;   ///< Serialises all writers.
} rcu_rb_root_t;

#define RCU_RB_ROOT_INIT  { .base = RB_ROOT_INIT, .lock = SPINLOCK_INIT }

/// @brief Lockless RCU reader search.  Must be called inside rcu_read_lock().
/// @param cmp  Returns >0 if target < node, 0 if equal, <0 if target > node.
rb_node_t *rcu_rb_find(const rcu_rb_root_t *root,
                        int (*cmp)(const rb_node_t *, const void *),
                        const void *arg);

/// @brief Writer insert (acquires root->lock internally).
void rcu_rb_insert(rcu_rb_root_t *root, rb_node_t *node,
                   rb_node_t *parent, rb_node_t **link);

/// @brief Writer erase (acquires root->lock, defers free via call_rcu).
void rcu_rb_erase(rcu_rb_root_t *root, rb_node_t *node,
                  rcu_head_t *head, void (*free_fn)(rcu_head_t *));

typedef struct {
    rb_root_aug_t          aug;   ///< Augmented base (contains rb_root_t).
    spinlock_t             lock;  ///< Serialises all writers.
} rcu_rb_root_aug_t;

#define RCU_RB_ROOT_AUG_INIT(callbacks) \
    { .aug = RB_ROOT_AUG_INIT(callbacks), .lock = SPINLOCK_INIT }

/// @brief Lockless RCU reader search on an augmented tree.
///        Identical to rcu_rb_find() — augmentation is invisible to readers.
rb_node_t *rcu_rb_aug_find(const rcu_rb_root_aug_t *root,
                            int (*cmp)(const rb_node_t *, const void *),
                            const void *arg);

/// @brief Writer insert into an RCU-augmented tree.
///        Acquires root->lock, inserts, propagates, publishes via
///        rcu_assign_pointer().
void rcu_rb_aug_insert(rcu_rb_root_aug_t *root, rb_node_t *node,
                       rb_node_t *parent, rb_node_t **link);

/// @brief Writer erase from an RCU-augmented tree.
///        Acquires root->lock, erases, propagates, defers free via call_rcu().
void rcu_rb_aug_erase(rcu_rb_root_aug_t *root, rb_node_t *node,
                      rcu_head_t *head, void (*free_fn)(rcu_head_t *));

/// @brief O(log n) gap search on an RCU-augmented tree.
///
///        Uses subtree_max_end to prune branches: a subtree whose
///        subtree_max_end < (cursor + size) cannot contain a large enough gap
///        left of any node in it, so it is skipped entirely.
///
///        node_end(n) must return the same value that propagate() uses as the
///        node's own endpoint contribution to subtree_max_end.
///
/// @param root        Augmented tree root.
/// @param size        Minimum gap size required.
/// @param align       Required alignment of the gap start (power of two).
/// @param lo          Search lower bound (inclusive).
/// @param hi          Search upper bound (exclusive).
/// @param node_start  Returns the start of the interval stored in node.
/// @param node_end    Returns the end   of the interval stored in node.
/// @return            Aligned gap start address, or 0 if none found.
uint64_t rcu_rb_aug_find_gap(const rcu_rb_root_aug_t *root,
                              uint64_t size, uint64_t align,
                              uint64_t lo,   uint64_t hi,
                              uint64_t (*node_start)(const rb_node_t *),
                              uint64_t (*node_end  )(const rb_node_t *));

// ---------------------------------------------------------------------------
// Layer 3 — Per-CPU hint cache with generation-counter invalidation
// ---------------------------------------------------------------------------

/// @brief Tree-side generation counter.  One per cached tree root.
///        Incremented atomically on every structural change (insert or erase).
///        Per-CPU hints store the generation at write time; a mismatch on
///        the next read discards the hint in O(1) without scanning all CPUs.
typedef struct {
    atomic64_t val;
} rb_cache_gen_t;

#define RB_CACHE_GEN_INIT  { .val = ATOMIC64_INIT(0) }

/// @brief Per-CPU search hint for a single rb-tree.
///
///        hint is opportunistic: it may be stale.
///        gen is compared against the tree's rb_cache_gen_t on every use;
///        a mismatch discards the hint in O(1).
///
///        The cache does NOT hold a reference.  The caller must ensure the
///        node cannot be freed while the hint is live (e.g., by holding
///        rcu_read_lock() or the write lock).
typedef struct {
    rb_node_t  *hint;
    uint64_t    hint_key;  ///< Caller-defined scalar key for the hint.
    uint64_t    gen;       ///< Generation at which hint was written.
} rb_pcpu_cache_t;

/// @brief Search a plain rb_root_t with a per-CPU hint and generation check.
///        Fast path O(1) on hit; slow path O(log n) on miss.
///        The miss path uses plain pointer loads — not RCU-safe.
///        Use rcu_rb_aug_find_cached() when inside rcu_read_lock().
rb_node_t *rb_find_cached(const rb_root_t *root,
                           rb_cache_gen_t  *gen,
                           rb_pcpu_cache_t  cache[],
                           int (*cmp)(const rb_node_t *, const void *),
                           const void *arg);

/// @brief RCU-correct search on an augmented tree with per-CPU hint.
///        Combines the O(1) generation-checked hint with an RCU-safe
///        (rcu_dereference on every pointer load) miss path.
///        Must be called inside rcu_read_lock().
rb_node_t *rcu_rb_aug_find_cached(const rcu_rb_root_aug_t *root,
                                   rb_cache_gen_t          *gen,
                                   rb_pcpu_cache_t          cache[],
                                   int (*cmp)(const rb_node_t *, const void *),
                                   const void *arg);

/// @brief Invalidate all per-CPU hints by bumping the generation counter.
///        O(1) — hints are lazily discarded on next use via gen comparison.
///        Must be called under the write lock on every structural change.
static inline void rb_cache_invalidate(rb_cache_gen_t *gen,
                                        rb_pcpu_cache_t cache[]) {
    (void)cache;
    atomic64_inc(&gen->val);
}

/// @brief Invalidate the hint on the current CPU only.  O(1).
void rb_cache_invalidate_local(rb_pcpu_cache_t cache[]);



