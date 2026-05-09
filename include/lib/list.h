// SPDX-License-Identifier: Apache-2.0
// include/lib/list.h
//
/// @brief Generic intrusive doubly linked list.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/common.h>     // container_of, container_of_const, __same_type

typedef struct list_node {
    struct list_node *next;
    struct list_node *prev;
} list_node_t;

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
    list_node_t name = LIST_HEAD_INIT(name)

static inline void list_init(list_node_t *ptr) {
    ptr->next = ptr;
    ptr->prev = ptr;
}

static inline void __list_add(list_node_t *new_node, list_node_t *prev, list_node_t *next) {
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

static inline void list_add_tail(list_node_t *new_node, list_node_t *head) {
    __list_add(new_node, head->prev, head);
}

static inline void list_add_head(list_node_t *new_node, list_node_t *head) {
    __list_add(new_node, head, head->next);
}

/// @brief Unlink @entry from its list and poison its pointers to nullptr.
///
///        After list_del() the node must NOT be passed to list_empty() or any
///        traversal macro — its next/prev are nullptr, not the list-head
///        sentinel.  Stale traversals will fault immediately rather than
///        silently walk garbage.  Re-insert the node with list_add_tail /
///        list_add_head; or use list_del_init() if re-linking is intended.
static inline void list_del(list_node_t *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = entry->prev = nullptr;
}

/// @brief Unlink @entry and re-initialize it to the empty (self-pointing) state.
///
///        Use this instead of list_del() whenever the node will be re-linked
///        later (e.g. magazine nodes that cycle between depot lists and the
///        CPU slot).  list_empty() on the node itself will return true after
///        this call, which is often the desired post-condition.
/// @param entry  Node to remove.
static inline void list_del_init(list_node_t *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = entry;
    entry->prev = entry;
}

/// @brief Test whether the list is empty (contains no nodes besides the head).
static inline bool list_empty(const list_node_t *head) {
    return head->next == head;
}

// ---------------------------------------------------------------------------
// list_entry / list_entry_const
//
// Both macros delegate to container_of / container_of_const from common.h,
// gaining the compile-time __same_type pointer-type check and standards-
// compliant offsetof arithmetic.
//
// The old &((type *)0)->member trick is UB under C23 §6.5.3.2p4 — it forms
// an lvalue through a null pointer.  GCC tolerates it (-fno-delete-null-
// pointer-checks), but Clang UBSAN type_mismatch_v1 fires on it, producing
// the null-pointer-dereference reports seen at slab.c:159.
// ---------------------------------------------------------------------------

/// @brief Recover the enclosing struct from a (non-const) pointer to a member.
/// @param ptr     Pointer to the embedded list_node_t.
/// @param type    Type of the enclosing struct.
/// @param member  Name of the list_node_t field within @type.
#define list_entry(ptr, type, member)       container_of(ptr, type, member)

/// @brief const-preserving variant; returns `const type *`.
/// @param ptr     const pointer to the embedded list_node_t.
/// @param type    Type of the enclosing struct.
/// @param member  Name of the list_node_t field within @type.
#define list_entry_const(ptr, type, member) container_of_const(ptr, type, member)

// ---------------------------------------------------------------------------
// Iteration helpers
// ---------------------------------------------------------------------------

/// @brief Forward iteration over typed entries (not safe to remove current node).
#define list_for_each_entry(pos, head, member) \
    for (pos = list_entry((head)->next, typeof(*pos), member); \
         &pos->member != (head); \
         pos = list_entry(pos->member.next, typeof(*pos), member))

/// @brief Forward iteration over typed entries; safe to remove the current node.
///
///        @tmp must be a pointer of the same type as @pos and is used
///        as a lookahead cursor to survive removal of @pos.
#define list_for_each_entry_safe(pos, tmp, head, member) \
    for (pos = list_entry((head)->next, typeof(*pos), member), \
         tmp = list_entry(pos->member.next, typeof(*pos), member); \
         &pos->member != (head); \
         pos = tmp, tmp = list_entry(tmp->member.next, typeof(*tmp), member))

/// @brief Raw list_node_t * forward iteration; safe to remove the current node.
///
///        Use this when you need the raw node pointer rather than a typed entry.
///        Both @pos and @tmp must be list_node_t *.
#define list_for_each_safe(pos, tmp, head) \
    for ((pos) = (head)->next, (tmp) = (pos)->next; \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = (pos)->next)
