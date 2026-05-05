// SPDX-License-Identifier: Apache-2.0
// include/lib/rbtree.h
//
/// @brief Intrusive red-black tree — freestanding, lock-free data structure.
///
///        DESIGN
///        ======
///        A classic left-leaning RB-tree with iterative (not recursive)
///        insert/delete to avoid kernel-stack overflows.  The caller embeds
///        rb_node_t inside their own struct and uses rb_entry() to recover
///        the container — identical to Linux's rbtree API style.
///
///        The tree itself (rb_root_t) stores only the root pointer and a
///        comparator function; the comparator returns negative / zero /
///        positive in the usual strcmp sense.
///
///        PROPERTIES
///        ==========
///        - O(log n) insert, delete, find.
///        - All operations are iterative (bounded stack depth = O(1) on heap).
///        - The colour bit is stored in the low bit of the parent pointer
///          (parent pointers are 8-byte-aligned on s390x), so rb_node_t is
///          exactly 24 bytes: left(8) + right(8) + parent_and_color(8).
///
///        THREAD SAFETY
///        =============
///        None built-in.  Callers (e.g. the VMM) protect the tree with their
///        own spinlock.

#pragma once

#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// rb_node_t — embed this inside the container struct
// ---------------------------------------------------------------------------

typedef struct rb_node {
    struct rb_node *left;
    struct rb_node *right;
    /// Parent pointer with the colour bit packed into bit 0.
    /// bit 0 = 0 → RED, bit 0 = 1 → BLACK.
    uintptr_t       parent_and_color;
} rb_node_t;

_Static_assert(sizeof(rb_node_t) == 24, "rb_node_t must be 24 bytes");

// ---------------------------------------------------------------------------
// rb_root_t — the tree root handle
// ---------------------------------------------------------------------------

typedef struct {
    rb_node_t *root;
} rb_root_t;

#define RB_ROOT_INIT    { .root = nullptr }

// ---------------------------------------------------------------------------
// Colour extraction / mutation
// ---------------------------------------------------------------------------

#define RB_RED    0U
#define RB_BLACK  1U

static inline rb_node_t *rb_parent(const rb_node_t *n) {
    return (rb_node_t *)(n->parent_and_color & ~(uintptr_t)1);
}

static inline unsigned rb_color(const rb_node_t *n) {
    return (unsigned)(n->parent_and_color & 1U);
}

static inline bool rb_is_red(const rb_node_t *n) {
    return rb_color(n) == RB_RED;
}

static inline bool rb_is_black(const rb_node_t *n) {
    return rb_color(n) == RB_BLACK;
}

static inline void rb_set_parent(rb_node_t *n, rb_node_t *p) {
    n->parent_and_color = (n->parent_and_color & 1U) | (uintptr_t)p;
}

static inline void rb_set_color(rb_node_t *n, unsigned color) {
    n->parent_and_color = (n->parent_and_color & ~(uintptr_t)1) | color;
}

static inline void rb_set_parent_color(rb_node_t *n, rb_node_t *p, unsigned color) {
    n->parent_and_color = (uintptr_t)p | color;
}

// ---------------------------------------------------------------------------
// Container recovery
// ---------------------------------------------------------------------------

/// @brief Recover the enclosing struct from an embedded rb_node_t pointer.
/// @param ptr     Pointer to the rb_node_t member.
/// @param type    Type of the enclosing struct.
/// @param member  Name of the rb_node_t field within type.
#define rb_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

/// @brief Null-safe rb_entry: returns nullptr if ptr is nullptr.
#define rb_entry_safe(ptr, type, member) \
    ({ typeof(ptr) _p = (ptr); _p ? rb_entry(_p, type, member) : nullptr; })

// ---------------------------------------------------------------------------
// Tree traversal
// ---------------------------------------------------------------------------

/// @brief Return the left-most (minimum) node in the tree, or nullptr.
rb_node_t *rb_first(const rb_root_t *tree);

/// @brief Return the right-most (maximum) node in the tree, or nullptr.
rb_node_t *rb_last(const rb_root_t *tree);

/// @brief Return the in-order successor of node, or nullptr.
rb_node_t *rb_next(const rb_node_t *node);

/// @brief Return the in-order predecessor of node, or nullptr.
rb_node_t *rb_prev(const rb_node_t *node);

// ---------------------------------------------------------------------------
// Insert / erase
// ---------------------------------------------------------------------------

/// @brief Link a new node into the tree at the given position.
///        The caller must:
///          1. Walk the tree manually (using the comparator) to find
///             the parent and which child slot (left/right) to use.
///          2. Call rb_link_node() to attach the new node.
///          3. Call rb_insert_fixup() to restore RB invariants.
/// @param node    The new (RED) node to insert.
/// @param parent  Parent node (nullptr if tree is empty).
/// @param link    &parent->left or &parent->right (or &tree->root).
void rb_link_node(rb_node_t *node, rb_node_t *parent, rb_node_t **link);

/// @brief Restore RB invariants after rb_link_node().
void rb_insert_fixup(rb_root_t *tree, rb_node_t *node);

/// @brief Remove a node from the tree, restoring RB invariants.
void rb_erase(rb_root_t *tree, rb_node_t *node);

// ---------------------------------------------------------------------------
// In-order iteration macros
// ---------------------------------------------------------------------------

#define rb_for_each(pos, tree) \
    for ((pos) = rb_first(tree); (pos); (pos) = rb_next(pos))

#define rb_for_each_entry(pos, tree, member) \
    for ((pos) = rb_entry_safe(rb_first(tree), __typeof__(*(pos)), member); \
         (pos); \
         (pos) = rb_entry_safe(rb_next(&(pos)->member), __typeof__(*(pos)), member))
