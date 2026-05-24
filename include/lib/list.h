// SPDX-License-Identifier: Apache-2.0
// include/lib/list.h
//
/// @brief Generic intrusive doubly-linked circular list.

#pragma once

#include <zxfoundation/common.h>

typedef struct list_head {
    struct list_head *next;
    struct list_head *prev;
} list_head_t;

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
    list_head_t name = LIST_HEAD_INIT(name)

static inline void list_init(list_head_t *h) {
    write_once(h->next, h);
    write_once(h->prev, h);
}

/// @brief Insert @n between @prev and @next.
///        The store order — next->prev, n->next, n->prev, prev->next — matches
///        Linux's __list_add and ensures that a concurrent RCU reader walking
///        forward (following ->next) never sees a partially-linked node:
///        the node is only reachable via prev->next after all its own fields
///        are written.
static inline void __list_add(list_head_t *n,
                               list_head_t *prev,
                               list_head_t *next) {
    write_once(next->prev, n);
    write_once(n->next,    next);
    write_once(n->prev,    prev);
    write_once(prev->next, n);
}

/// @brief Insert @n at the head of the list (after the sentinel).
///        O(1).  Use for LIFO / stack semantics.
static inline void list_add(list_head_t *n, list_head_t *head) {
    __list_add(n, head, read_once(head->next));
}

/// @brief Insert @n at the tail of the list (before the sentinel).
///        O(1).  Use for FIFO / queue semantics.
static inline void list_add_tail(list_head_t *n, list_head_t *head) {
    __list_add(n, read_once(head->prev), head);
}

/// @brief Unlink @n from its list.
///        Poisons next/prev to nullptr so any stale traversal faults
///        immediately rather than silently walking garbage.
///        Do NOT call list_empty() on a deleted node — use list_del_init()
///        if the node will be re-inserted later.
static inline void list_del(list_head_t *n) {
    list_head_t *prev = read_once(n->prev);
    list_head_t *next = read_once(n->next);
    write_once(prev->next, next);
    write_once(next->prev, prev);
    write_once(n->next, nullptr);
    write_once(n->prev, nullptr);
}

/// @brief Unlink @n and reinitialize it to the empty (self-pointing) state.
///        list_empty() returns true after this call.
///        Use whenever the node will be re-linked later.
static inline void list_del_init(list_head_t *n) {
    list_head_t *prev = read_once(n->prev);
    list_head_t *next = read_once(n->next);
    write_once(prev->next, next);
    write_once(next->prev, prev);
    write_once(n->next, n);
    write_once(n->prev, n);
}

/// @brief Replace @old with @n in the list.
///        @old is left with poisoned pointers (nullptr).
///        @n must not already be in a list.
static inline void list_replace(list_head_t *old, list_head_t *n) {
    write_once(n->next, read_once(old->next));
    write_once(read_once(n->next)->prev, n);
    write_once(n->prev, read_once(old->prev));
    write_once(read_once(n->prev)->next, n);
    write_once(old->next, nullptr);
    write_once(old->prev, nullptr);
}

/// @brief Replace @old with @n and reinitialize @old to empty.
static inline void list_replace_init(list_head_t *old, list_head_t *n) {
    list_replace(old, n);
    list_init(old);
}

/// @brief Move @n to the head of @head.
///        Equivalent to list_del(@n) + list_add(@n, @head) but expressed
///        as a single operation for clarity at call sites.
static inline void list_move(list_head_t *n, list_head_t *head) {
    list_del(n);
    list_add(n, head);
}

/// @brief Move @n to the tail of @head.
static inline void list_move_tail(list_head_t *n, list_head_t *head) {
    list_del(n);
    list_add_tail(n, head);
}

/// @brief Move all nodes from @list to the tail of @head, leaving @list empty.
///        @list must not be empty.  O(1).
static inline void list_bulk_move_tail(list_head_t *head, list_head_t *list) {
    list_head_t *first = read_once(list->next);
    list_head_t *last  = read_once(list->prev);
    list_head_t *at    = read_once(head->prev);

    write_once(first->prev, at);
    write_once(at->next,    first);
    write_once(last->next,  head);
    write_once(head->prev,  last);
    list_init(list);
}
/// @brief True if the list contains no entries.
static inline bool list_empty(const list_head_t *head) {
    return read_once(head->next) == head;
}

/// @brief Careful empty check: reads both next and prev.
///        Returns true only if both point back to head.
///        Use when another CPU may be concurrently deleting the last entry
///        (i.e., when you cannot hold the list lock but need a safe snapshot).
static inline bool list_empty_careful(const list_head_t *head) {
    list_head_t *next = read_once(head->next);
    return (next == head) && (next == read_once(head->prev));
}

/// @brief True if the list contains exactly one entry.
static inline bool list_is_singular(const list_head_t *head) {
    list_head_t *next = read_once(head->next);
    return (next != head) && (next == read_once(head->prev));
}

/// @brief True if @n is the last entry in the list (its next is the head).
static inline bool list_is_last(const list_head_t *n, const list_head_t *head) {
    return read_once(n->next) == head;
}

/// @brief True if @n is the first entry in the list (its prev is the head).
static inline bool list_is_first(const list_head_t *n, const list_head_t *head) {
    return read_once(n->prev) == head;
}

/// @brief True if @n is currently linked into a list (not self-pointing or null).
///        Only valid after list_del_init() or list_init() — not after list_del().
static inline bool list_is_linked(const list_head_t *n) {
    return read_once(n->next) != n;
}

/// @brief Count the number of entries in the list.  O(n).
///        Use only for diagnostics and assertions — never on hot paths.
static inline size_t list_count_nodes(const list_head_t *head) {
    size_t count = 0;
    const list_head_t *pos = read_once(head->next);
    while (pos != head) {
        count++;
        pos = read_once(pos->next);
    }
    return count;
}

/// @brief Rotate the list left: move the first entry to the tail.
///        No-op on an empty list.  O(1).
static inline void list_rotate_left(list_head_t *head) {
    if (!list_empty(head))
        list_move_tail(read_once(head->next), head);
}

/// @brief Rotate the list so that @n becomes the new first entry.
///        @n must be in the list.  O(1).
static inline void list_rotate_to_front(list_head_t *n, list_head_t *head) {
    list_move_tail(read_once(head->prev), head);
    list_move(n, head);
}

/// @brief Join @list into @head after position @prev.
///        Internal helper — does not reinitialize @list.
static inline void __list_splice(const list_head_t *list,
                                  list_head_t *prev,
                                  list_head_t *next) {
    list_head_t *first = read_once(list->next);
    list_head_t *last  = read_once(list->prev);

    write_once(first->prev, prev);
    write_once(prev->next,  first);
    write_once(last->next,  next);
    write_once(next->prev,  last);
}

/// @brief Splice @list at the head of @head.
///        @list is left in an undefined state — use list_splice_init() if
///        @list will be reused.
static inline void list_splice(list_head_t *list, list_head_t *head) {
    if (!list_empty(list))
        __list_splice(list, head, read_once(head->next));
}

/// @brief Splice @list at the tail of @head.
static inline void list_splice_tail(list_head_t *list, list_head_t *head) {
    if (!list_empty(list))
        __list_splice(list, read_once(head->prev), head);
}

/// @brief Splice @list at the head of @head and reinitialize @list to empty.
static inline void list_splice_init(list_head_t *list, list_head_t *head) {
    if (!list_empty(list)) {
        __list_splice(list, head, read_once(head->next));
        list_init(list);
    }
}

/// @brief Splice @list at the tail of @head and reinitialize @list to empty.
static inline void list_splice_tail_init(list_head_t *list, list_head_t *head) {
    if (!list_empty(list)) {
        __list_splice(list, read_once(head->prev), head);
        list_init(list);
    }
}

/// @brief Recover the enclosing struct from a (non-const) pointer to a member.
#define list_entry(ptr, type, member)       container_of(ptr, type, member)

/// @brief const-preserving variant; returns `const type *`.
#define list_entry_const(ptr, type, member) container_of_const(ptr, type, member)

/// @brief First entry in the list.  Undefined behavior if the list is empty.
#define list_first_entry(head, type, member) \
    list_entry(read_once((head)->next), type, member)

/// @brief Last entry in the list.  Undefined behavior if the list is empty.
#define list_last_entry(head, type, member) \
    list_entry(read_once((head)->prev), type, member)

/// @brief First entry, or nullptr if the list is empty.
#define list_first_entry_or_null(head, type, member) ({         \
    list_head_t *__h = (head);                                   \
    list_head_t *__n = read_once(__h->next);                     \
    (__n != __h) ? list_entry(__n, type, member) : nullptr;      \
})

/// @brief Last entry, or nullptr if the list is empty.
#define list_last_entry_or_null(head, type, member) ({           \
    list_head_t *__h = (head);                                   \
    list_head_t *__p = read_once(__h->prev);                     \
    (__p != __h) ? list_entry(__p, type, member) : nullptr;      \
})

/// @brief Next entry after @pos.  Does not check for end-of-list.
#define list_next_entry(pos, member) \
    list_entry(read_once((pos)->member.next), typeof(*(pos)), member)

/// @brief Previous entry before @pos.  Does not check for end-of-list.
#define list_prev_entry(pos, member) \
    list_entry(read_once((pos)->member.prev), typeof(*(pos)), member)

/// @brief Next entry, or nullptr if @pos is the last entry.
#define list_next_entry_or_null(pos, head, member) ({                       \
    list_head_t *__n = read_once((pos)->member.next);                       \
    (__n != (head)) ? list_entry(__n, typeof(*(pos)), member) : nullptr;    \
})

/// @brief Forward iteration over raw list_node_t * (not safe to remove current).
#define list_for_each(pos, head) \
    for ((pos) = read_once((head)->next); \
         (pos) != (head); \
         (pos) = read_once((pos)->next))

/// @brief Forward iteration over raw list_node_t *; safe to remove current node.
#define list_for_each_safe(pos, tmp, head) \
    for ((pos) = read_once((head)->next), (tmp) = read_once((pos)->next); \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = read_once((pos)->next))

/// @brief Backward iteration over raw list_node_t * (not safe to remove current).
#define list_for_each_prev(pos, head) \
    for ((pos) = read_once((head)->prev); \
         (pos) != (head); \
         (pos) = read_once((pos)->prev))

/// @brief Backward iteration; safe to remove current node.
#define list_for_each_prev_safe(pos, tmp, head) \
    for ((pos) = read_once((head)->prev), (tmp) = read_once((pos)->prev); \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = read_once((pos)->prev))

/// @brief Forward iteration over typed entries (not safe to remove current).
#define list_for_each_entry(pos, head, member) \
    for ((pos) = list_first_entry(head, typeof(*(pos)), member); \
         &(pos)->member != (head); \
         (pos) = list_next_entry(pos, member))

/// @brief Forward iteration over typed entries; safe to remove current node.
#define list_for_each_entry_safe(pos, tmp, head, member) \
    for ((pos) = list_first_entry(head, typeof(*(pos)), member), \
         (tmp) = list_next_entry(pos, member); \
         &(pos)->member != (head); \
         (pos) = (tmp), (tmp) = list_next_entry(tmp, member))

/// @brief Continue forward iteration from @pos (exclusive — starts at pos->next).
///        @pos must already be a valid entry in the list.
#define list_for_each_entry_from(pos, head, member) \
    for (; &(pos)->member != (head); \
         (pos) = list_next_entry(pos, member))

/// @brief Continue forward iteration from @pos, safe to remove current node.
#define list_for_each_entry_from_safe(pos, tmp, head, member) \
    for ((tmp) = list_next_entry(pos, member); \
         &(pos)->member != (head); \
         (pos) = (tmp), (tmp) = list_next_entry(tmp, member))

/// @brief Backward iteration over typed entries (not safe to remove current).
#define list_for_each_entry_reverse(pos, head, member) \
    for ((pos) = list_last_entry(head, typeof(*(pos)), member); \
         &(pos)->member != (head); \
         (pos) = list_prev_entry(pos, member))

/// @brief Backward iteration over typed entries; safe to remove current node.
#define list_for_each_entry_reverse_safe(pos, tmp, head, member) \
    for ((pos) = list_last_entry(head, typeof(*(pos)), member), \
         (tmp) = list_prev_entry(pos, member); \
         &(pos)->member != (head); \
         (pos) = (tmp), (tmp) = list_prev_entry(tmp, member))
