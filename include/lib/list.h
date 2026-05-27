// SPDX-License-Identifier: Apache-2.0
// include/lib/list.h
//
/// @brief Generic intrusive doubly-linked circular list and its variants.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/common.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sync/rcu.h>
#include <arch/s390x/cpu/atomic.h>

typedef struct list_head {
    struct list_head *next;
    struct list_head *prev;
} list_head_t;

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
    list_head_t name = LIST_HEAD_INIT(name)

/// @brief Initialize (or reinitialize) @h to the empty, self-pointing state.
static inline void list_init(list_head_t *h) {
    write_once(h->next, h);
    write_once(h->prev, h);
}

/// @brief Report and halt on an add-path corruption.  Never returns.
bool __list_add_valid_or_report(list_head_t *new,
                                list_head_t *prev,
                                list_head_t *next);

/// @brief Report and halt on a del-path corruption.  Never returns.
bool __list_del_entry_valid_or_report(list_head_t *entry);


/// @brief True if the list has no entries (head->next == head).
static inline bool list_empty(const list_head_t *head) {
    return read_once(head->next) == head;
}

/// @brief Validate the three-pointer invariant before insertion.
///
///        The fast path checks only the two structural predicates:
///        next->prev == prev and prev->next == next.  Both must hold for
///        a well-formed insertion point.  If either fails, the corruption
///        reporter is called, which issues a ZX_SYSCHK_CORE_CORRUPT and
///        halts all CPUs.
///
/// @return true if the invariant holds, false otherwise.
static inline bool __list_add_valid(list_head_t *new,
                                    list_head_t *prev,
                                    list_head_t *next) {
    bool ret = true;

    if (likely(next->prev == prev && prev->next == next &&
               new != prev    && new != next))
        return true;
    ret = false;

    ret &= __list_add_valid_or_report(new, prev, next);
    return ret;
}

/// @brief Validate the self-consistency of @entry before deletion.
///
///        Checks that entry->prev->next == entry and entry->next->prev ==
///        entry.  A failure indicates that either the node was already
///        deleted, or that a concurrent writer corrupted the neighbouring
///        pointers without holding the appropriate lock.
///
/// @return true if the invariant holds, false otherwise.
static inline bool __list_del_entry_valid(list_head_t *entry) {
    bool ret = true;

    const list_head_t *prev = entry->prev;
    const list_head_t *next = entry->next;

    if (likely(prev->next == entry && next->prev == entry))
        return true;
    ret = false;

    ret &= __list_del_entry_valid_or_report(entry);
    return ret;
}

/// @brief Insert @n between @prev and @next.
///
///        Store ordering — next->prev, n->next, n->prev, prev->next —
///        matches Linux's __list_add().  A concurrent lockless RCU reader
///        walking forward (following ->next) will never observe a partially-
///        linked node: @n is only reachable via prev->next after all of its
///        own pointer fields are written.
static inline void __list_add(list_head_t *n,
                               list_head_t *prev,
                               list_head_t *next) {
    if (!__list_add_valid(n, prev, next))
        return;
    write_once(next->prev, n);
    write_once(n->next,    next);
    write_once(n->prev,    prev);
    write_once(prev->next, n);
}

/// @brief Unlink @prev and @next from each other without touching the
///        removed node's own pointers.  The caller is responsible for
///        poisoning or reinitializing the removed node afterward.
static inline void __list_del(list_head_t *prev, list_head_t *next) {
    write_once(prev->next, next);
    write_once(next->prev, prev);
}

/// @brief Insert @n immediately after @head (LIFO / stack semantics).  O(1).
static inline void list_add(list_head_t *n, list_head_t *head) {
    __list_add(n, head, read_once(head->next));
}

/// @brief Insert @n immediately before @head (FIFO / queue semantics).  O(1).
static inline void list_add_tail(list_head_t *n, list_head_t *head) {
    __list_add(n, read_once(head->prev), head);
}

/// @brief Unlink @n and poison its pointers to nullptr.
///
///        After this call, list_empty() is undefined on @n.  Use
///        list_del_init() if the node will be re-inserted into a list later.
///        The nullptr poison ensures that any stale traversal through the
///        deleted node faults immediately rather than silently walking garbage.
static inline void list_del(list_head_t *n) {
    if (!__list_del_entry_valid(n))
        return;
    list_head_t *prev = read_once(n->prev);
    list_head_t *next = read_once(n->next);
    __list_del(prev, next);
    write_once(n->next, nullptr);
    write_once(n->prev, nullptr);
}

/// @brief Unlink @n and reinitialize it to the empty, self-pointing state.
///        list_empty() returns true after this call.
///        Use whenever the node will be re-inserted later.
static inline void list_del_init(list_head_t *n) {
    if (!__list_del_entry_valid(n))
        return;
    list_head_t *prev = read_once(n->prev);
    list_head_t *next = read_once(n->next);
    __list_del(prev, next);
    write_once(n->next, n);
    write_once(n->prev, n);
}

/// @brief Replace @old with @n in its list.
///        @old is left with nullptr pointers.
///        @n must not already be linked into any list.
static inline void list_replace(list_head_t *old, list_head_t *n) {
    write_once(n->next,                  read_once(old->next));
    write_once(read_once(n->next)->prev, n);
    write_once(n->prev,                  read_once(old->prev));
    write_once(read_once(n->prev)->next, n);
    write_once(old->next, nullptr);
    write_once(old->prev, nullptr);
}

/// @brief Replace @old with @n and reinitialize @old to the empty state.
static inline void list_replace_init(list_head_t *old, list_head_t *n) {
    list_replace(old, n);
    list_init(old);
}

/// @brief Remove @n from its current list and reinsert at the head of @head.
static inline void list_move(list_head_t *n, list_head_t *head) {
    list_del(n);
    list_add(n, head);
}

/// @brief Remove @n from its current list and reinsert at the tail of @head.
static inline void list_move_tail(list_head_t *n, list_head_t *head) {
    list_del(n);
    list_add_tail(n, head);
}

/// @brief Atomically graft all entries from @list to the tail of @head.
///        @list is left empty afterward.  No-op if @list is empty.  O(1).
///
///        The six-store sequence keeps both lists consistent at every
///        intermediate step, so a concurrent RCU reader on @head sees
///        either the old tail or the new segment — never a broken link.
static inline void list_bulk_move_tail(list_head_t *head, list_head_t *list) {
    if (list_empty(list))
        return;

    list_head_t *first = read_once(list->next);
    list_head_t *last  = read_once(list->prev);
    list_head_t *at    = read_once(head->prev);

    write_once(first->prev, at);
    write_once(at->next,    first);
    write_once(last->next,  head);
    write_once(head->prev,  last);
    list_init(list);
}

/// @brief Partition @head: move the sublist [head->next .. @entry] into @list.
///
///        @list must be an initialized empty list.  After the call:
///          - @list contains [original head->next .. @entry]
///          - @head retains  [@entry->next .. original tail]
///
///        The caller must guarantee that @entry is actually a member of @head.
///        Passing an @entry from a different list corrupts both lists without
///        any runtime detection.  O(1).
static inline void list_cut_position(list_head_t *list,
                                     list_head_t *head,
                                     list_head_t *entry) {
    if (list_empty(head))
        return;
    if (head == entry) {
        list_init(list);
        return;
    }

    list_head_t *new_first = read_once(entry->next);

    write_once(list->next,              read_once(head->next));
    write_once(read_once(list->next)->prev, list);
    write_once(list->prev,              entry);
    write_once(entry->next,             list);
    write_once(head->next,              new_first);
    write_once(new_first->prev,         head);
}

/// @brief Partition @head: move the sublist [head->next .. @entry->prev]
///        into @list.  @entry itself remains as the new first element of @head.
///
///        If @entry is already the first element, @list is left empty.  O(1).
static inline void list_cut_before(list_head_t *list,
                                   list_head_t *head,
                                   list_head_t *entry) {
    if (read_once(head->next) == entry) {
        list_init(list);
        return;
    }

    write_once(list->next,                   read_once(head->next));
    write_once(read_once(list->next)->prev,  list);
    write_once(list->prev,                   read_once(entry->prev));
    write_once(read_once(list->prev)->next,  list);
    write_once(head->next,                   entry);
    write_once(entry->prev,                  head);
}

/// @brief Careful empty check: reads both ->next and ->prev.
///
///        Returns true only when both pointers refer back to @head.
///        Use when another CPU may be concurrently deleting the last entry
///        and no list lock is held — this double-read detects the window
///        where next has been redirected but prev has not yet been updated.
static inline bool list_empty_careful(const list_head_t *head) {
    list_head_t *next = read_once(head->next);
    return (next == head) && (next == read_once(head->prev));
}

/// @brief True if the list contains exactly one entry.
static inline bool list_is_singular(const list_head_t *head) {
    list_head_t *next = read_once(head->next);
    return (next != head) && (next == read_once(head->prev));
}

/// @brief True if @n is the last entry in @head (n->next == head).
static inline bool list_is_last(const list_head_t *n, const list_head_t *head) {
    return read_once(n->next) == head;
}

/// @brief True if @n is the first entry in @head (n->prev == head).
static inline bool list_is_first(const list_head_t *n, const list_head_t *head) {
    return read_once(n->prev) == head;
}

/// @brief True if @n is linked into a list (not self-pointing, not poisoned).
///        Valid only after list_del_init() or list_init() — NOT after list_del().
static inline bool list_is_linked(const list_head_t *n) {
    return read_once(n->next) != n;
}

/// @brief Count entries in the list.  O(n).
///        Use only in diagnostic paths and assertions — never on hot paths.
static inline size_t list_count_nodes(const list_head_t *head) {
    size_t count = 0;
    const list_head_t *pos = read_once(head->next);
    while (pos != head) {
        count++;
        pos = read_once(pos->next);
    }
    return count;
}

/// @brief Rotate left: move the first entry to the tail.  No-op if empty. O(1).
static inline void list_rotate_left(list_head_t *head) {
    if (!list_empty(head))
        list_move_tail(read_once(head->next), head);
}

/// @brief Rotate so that @n becomes the new first entry.  O(1).
///        @n must already be a member of @head.
static inline void list_rotate_to_front(list_head_t *n, list_head_t *head) {
    list_move_tail(read_once(head->prev), head);
    list_move(n, head);
}

/// @brief Internal splice helper — does not reinitialize @list.
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
///        @list will be reused afterward.
static inline void list_splice(list_head_t *list, list_head_t *head) {
    if (!list_empty(list))
        __list_splice(list, head, read_once(head->next));
}

/// @brief Splice @list at the tail of @head.
static inline void list_splice_tail(list_head_t *list, list_head_t *head) {
    if (!list_empty(list))
        __list_splice(list, read_once(head->prev), head);
}

/// @brief Splice @list at the head of @head; reinitialize @list to empty.
static inline void list_splice_init(list_head_t *list, list_head_t *head) {
    if (!list_empty(list)) {
        __list_splice(list, head, read_once(head->next));
        list_init(list);
    }
}

/// @brief Splice @list at the tail of @head; reinitialize @list to empty.
static inline void list_splice_tail_init(list_head_t *list, list_head_t *head) {
    if (!list_empty(list)) {
        __list_splice(list, read_once(head->prev), head);
        list_init(list);
    }
}

/// @brief Comparator type for list_add_sorted() and list_sort().
///        Must return: < 0 if @a < @b, 0 if equal, > 0 if @a > @b.
typedef int (*list_cmp_fn_t)(const list_head_t *a, const list_head_t *b);

/// @brief Insert @n into @head maintaining ascending order per @cmp.  O(n).
///
///        @head must already be sorted.  The insert is stable: @n is placed
///        after all existing entries that compare equal to it.
///        Must be called under the appropriate external lock for SMP safety.
void list_add_sorted(list_head_t *n, list_head_t *head, list_cmp_fn_t cmp);

/// @brief Sort @head in-place with a stable bottom-up merge sort.  O(n log n).
///
///        No memory allocation is performed.  The list head is used only as
///        the circular sentinel; entries are fully relinked.  Based on
///        Simon Tatham's "Sorting in C" and the Linux kernel list_sort().
///
///        Must be called under the appropriate external lock for SMP safety
///        (see Drawback D-3 in the file header).
void list_sort(list_head_t *head, list_cmp_fn_t cmp);

/// @brief Detect an interior cycle using Floyd's two-pointer algorithm.  O(n).
///
///        Returns true if walking ->next from @head never reaches @head
///        (indicating that some node's ->next points back to an interior node
///        rather than to the expected successor, or that a nullptr was
///        encountered mid-walk).
///
///        See Drawback D-4: this function holds no lock.  Use only in
///        diagnostic and assertion paths under external locking.
bool list_detect_cycle(const list_head_t *head);

/// @brief Recover the enclosing struct from an embedded list_head_t pointer.
#define list_entry(ptr, type, member)       container_of(ptr, type, member)

/// @brief const-preserving variant; returns `const type *`.
#define list_entry_const(ptr, type, member) container_of_const(ptr, type, member)

/// @brief First entry.  Undefined behavior if the list is empty.
#define list_first_entry(head, type, member) \
    list_entry(read_once((head)->next), type, member)

/// @brief Last entry.  Undefined behavior if the list is empty.
#define list_last_entry(head, type, member) \
    list_entry(read_once((head)->prev), type, member)

/// @brief First entry, or nullptr if the list is empty.
#define list_first_entry_or_null(head, type, member) ({        \
    list_head_t *__h = (head);                                  \
    list_head_t *__n = read_once(__h->next);                    \
    (__n != __h) ? list_entry(__n, type, member) : nullptr;     \
})

/// @brief Last entry, or nullptr if the list is empty.
#define list_last_entry_or_null(head, type, member) ({          \
    list_head_t *__h = (head);                                  \
    list_head_t *__p = read_once(__h->prev);                    \
    (__p != __h) ? list_entry(__p, type, member) : nullptr;     \
})

/// @brief Next entry after @pos.  Does not check for end-of-list.
#define list_next_entry(pos, member) \
    list_entry(read_once((pos)->member.next), typeof(*(pos)), member)

/// @brief Previous entry before @pos.  Does not check for end-of-list.
#define list_prev_entry(pos, member) \
    list_entry(read_once((pos)->member.prev), typeof(*(pos)), member)

/// @brief Next entry after @pos, or nullptr if @pos is the last entry.
#define list_next_entry_or_null(pos, head, member) ({                       \
    list_head_t *__n = read_once((pos)->member.next);                        \
    (__n != (head)) ? list_entry(__n, typeof(*(pos)), member) : nullptr;     \
})

/// @brief Forward iteration over raw list_head_t *; not safe to remove current.
#define list_for_each(pos, head) \
    for ((pos) = read_once((head)->next); \
         (pos) != (head); \
         (pos) = read_once((pos)->next))

/// @brief Forward iteration; safe to remove the current node.
///
///        @tmp caches (pos)->next before the loop body runs, so that
///        list_del(pos) inside the body does not corrupt the walk.
#define list_for_each_safe(pos, tmp, head) \
    for ((pos) = read_once((head)->next), (tmp) = read_once((pos)->next); \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = read_once((pos)->next))

/// @brief Backward iteration; not safe to remove current.
#define list_for_each_prev(pos, head) \
    for ((pos) = read_once((head)->prev); \
         (pos) != (head); \
         (pos) = read_once((pos)->prev))

/// @brief Backward iteration; safe to remove current node.
#define list_for_each_prev_safe(pos, tmp, head) \
    for ((pos) = read_once((head)->prev), (tmp) = read_once((pos)->prev); \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = read_once((pos)->prev))

/// @brief Forward iteration over typed entries; not safe to remove current.
#define list_for_each_entry(pos, head, member) \
    for ((pos) = list_first_entry(head, typeof(*(pos)), member); \
         &(pos)->member != (head); \
         (pos) = list_next_entry(pos, member))

/// @brief Forward iteration over typed entries; safe to remove current.
#define list_for_each_entry_safe(pos, tmp, head, member) \
    for ((pos) = list_first_entry(head, typeof(*(pos)), member), \
         (tmp) = list_next_entry(pos, member); \
         &(pos)->member != (head); \
         (pos) = (tmp), (tmp) = list_next_entry(tmp, member))

/// @brief Continue forward from @pos (exclusive — starts at pos->next).
///        @pos must already be a valid entry in the list.
#define list_for_each_entry_from(pos, head, member) \
    for (; &(pos)->member != (head); \
         (pos) = list_next_entry(pos, member))

/// @brief Continue forward from @pos, safe to remove current.
#define list_for_each_entry_from_safe(pos, tmp, head, member) \
    for ((tmp) = list_next_entry(pos, member); \
         &(pos)->member != (head); \
         (pos) = (tmp), (tmp) = list_next_entry(tmp, member))

/// @brief Continue forward from the entry after @pos (inclusive of next).
///        The current @pos is skipped; iteration starts at pos->next.
///        Analogous to a C++ iterator's ++it before the loop body.
#define list_for_each_entry_continue(pos, head, member) \
    for ((pos) = list_next_entry(pos, member); \
         &(pos)->member != (head); \
         (pos) = list_next_entry(pos, member))

/// @brief Continue forward from pos->next, safe to remove current.
#define list_for_each_entry_continue_safe(pos, tmp, head, member) \
    for ((pos) = list_next_entry(pos, member), \
         (tmp) = list_next_entry(pos, member); \
         &(pos)->member != (head); \
         (pos) = (tmp), (tmp) = list_next_entry(tmp, member))

/// @brief Backward iteration over typed entries; not safe to remove current.
#define list_for_each_entry_reverse(pos, head, member) \
    for ((pos) = list_last_entry(head, typeof(*(pos)), member); \
         &(pos)->member != (head); \
         (pos) = list_prev_entry(pos, member))

/// @brief Backward iteration over typed entries; safe to remove current.
#define list_for_each_entry_reverse_safe(pos, tmp, head, member) \
    for ((pos) = list_last_entry(head, typeof(*(pos)), member), \
         (tmp) = list_prev_entry(pos, member); \
         &(pos)->member != (head); \
         (pos) = (tmp), (tmp) = list_prev_entry(tmp, member))

/// @brief Continue backward from the entry before @pos.
///        The current @pos is skipped; iteration starts at pos->prev.
#define list_for_each_entry_continue_reverse(pos, head, member) \
    for ((pos) = list_prev_entry(pos, member); \
         &(pos)->member != (head); \
         (pos) = list_prev_entry(pos, member))

/// @brief RCU-safe insert @n at the head of @head.
static inline void list_add_rcu(list_head_t *n, list_head_t *head) {
    list_head_t *next = read_once(head->next);
    write_once(n->prev,   head);
    write_once(n->next,   next);
    write_once(next->prev, n);
    rcu_assign_pointer(head->next, n);
}

/// @brief RCU-safe insert @n at the tail of @head.
static inline void list_add_tail_rcu(list_head_t *n, list_head_t *head) {
    list_head_t *prev = read_once(head->prev);
    write_once(n->next,   head);
    write_once(n->prev,   prev);
    write_once(prev->next, n);
    rcu_assign_pointer(head->prev, n);
}

/// @brief RCU-safe unlink @n.
///
///        Does NOT poison ->next / ->prev (see Protocol P-2 above).
///        The writer must call synchronize_rcu() or call_rcu() before
///        freeing or reinitializing @n.
static inline void list_del_rcu(list_head_t *n) {
    list_head_t *prev = read_once(n->prev);
    list_head_t *next = read_once(n->next);
    __list_del(prev, next);
}

/// @brief RCU-safe in-place replacement of @old with @n.
///
///        @old is left with its pointers intact for in-flight readers.
///        @n must not be linked into any list before this call.
static inline void list_replace_rcu(list_head_t *old, list_head_t *n) {
    write_once(n->next,                  read_once(old->next));
    write_once(n->prev,                  read_once(old->prev));
    write_once(read_once(n->next)->prev, n);
    rcu_assign_pointer(read_once(n->prev)->next, n);
}

/// @brief Forward RCU-safe iteration over raw list_head_t *.
#define list_for_each_rcu(pos, head) \
    for ((pos) = rcu_dereference((head)->next); \
         (pos) != (head); \
         (pos) = rcu_dereference((pos)->next))

/// @brief Forward RCU-safe iteration; safe to delete current with list_del_rcu().
#define list_for_each_safe_rcu(pos, tmp, head) \
    for ((pos) = rcu_dereference((head)->next), \
         (tmp) = rcu_dereference((pos)->next); \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = rcu_dereference((pos)->next))

/// @brief Forward RCU-safe iteration over typed entries.
#define list_for_each_entry_rcu(pos, head, member) \
    for ((pos) = list_entry(rcu_dereference((head)->next),         \
                            typeof(*(pos)), member);               \
         &(pos)->member != (head);                                 \
         (pos) = list_entry(rcu_dereference((pos)->member.next),   \
                            typeof(*(pos)), member))

/// @brief Forward RCU-safe iteration over typed entries; safe to delete current.
#define list_for_each_entry_safe_rcu(pos, tmp, head, member) \
    for ((pos) = list_entry(rcu_dereference((head)->next),         \
                            typeof(*(pos)), member),               \
         (tmp) = list_next_entry(pos, member);                     \
         &(pos)->member != (head);                                 \
         (pos) = (tmp), (tmp) = list_next_entry(tmp, member))

typedef struct {
    list_head_t head; ///< Sentinel node — do NOT embed in any entry struct.
    spinlock_t lock; ///< Protects all mutations to this list.
} list_lk_t;

#define LIST_LK_INIT(name) \
    { .head = LIST_HEAD_INIT((name).head), .lock = SPINLOCK_INIT }

#define DEFINE_LIST_LK(name)  list_lk_t name = LIST_LK_INIT(name)

/// @brief Initialize a list_lk_t at runtime.
static inline void list_lk_init(list_lk_t *lk) {
    list_init(&lk->head);
    spin_lock_init(&lk->lock);
}

/// @brief Insert @n at the head of @lk.  Acquires @lk->lock with irqsave.
static inline void list_lk_add(list_head_t *n, list_lk_t *lk) {
    irqflags_t flags;
    spin_lock_irqsave(&lk->lock, &flags);
    list_add(n, &lk->head);
    spin_unlock_irqrestore(&lk->lock, flags);
}

/// @brief Insert @n at the tail of @lk.  Acquires @lk->lock with irqsave.
static inline void list_lk_add_tail(list_head_t *n, list_lk_t *lk) {
    irqflags_t flags;
    spin_lock_irqsave(&lk->lock, &flags);
    list_add_tail(n, &lk->head);
    spin_unlock_irqrestore(&lk->lock, flags);
}

/// @brief Remove @n from its list under @lk's lock.  Poisons @n's pointers.
static inline void list_lk_del(list_head_t *n, list_lk_t *lk) {
    irqflags_t flags;
    spin_lock_irqsave(&lk->lock, &flags);
    list_del(n);
    spin_unlock_irqrestore(&lk->lock, flags);
}

/// @brief Remove @n and reinitialize it to empty under @lk's lock.
static inline void list_lk_del_init(list_head_t *n, list_lk_t *lk) {
    irqflags_t flags;
    spin_lock_irqsave(&lk->lock, &flags);
    list_del_init(n);
    spin_unlock_irqrestore(&lk->lock, flags);
}

/// @brief Rotate @lk left (first entry moves to tail) under its lock.  O(1).
static inline void list_lk_rotate_left(list_lk_t *lk) {
    irqflags_t flags;
    spin_lock_irqsave(&lk->lock, &flags);
    list_rotate_left(&lk->head);
    spin_unlock_irqrestore(&lk->lock, flags);
}

/// @brief True if @lk has no entries.  Acquires the lock for a consistent read.
static inline bool list_lk_empty(list_lk_t *lk) {
    irqflags_t flags;
    spin_lock_irqsave(&lk->lock, &flags);
    bool empty = list_empty(&lk->head);
    spin_unlock_irqrestore(&lk->lock, flags);
    return empty;
}

/// @brief True if @lk has exactly one entry.  Acquires the lock.
static inline bool list_lk_is_singular(list_lk_t *lk) {
    irqflags_t flags;
    spin_lock_irqsave(&lk->lock, &flags);
    bool singular = list_is_singular(&lk->head);
    spin_unlock_irqrestore(&lk->lock, flags);
    return singular;
}

/// @brief Atomically pop the first entry; returns nullptr if empty.
///        Acquires @lk->lock.  O(1).
/// @param lk      Pointer to the list_lk_t.
/// @param type    Enclosing struct type.
/// @param member  Name of the list_head_t member inside @type.
#define list_lk_pop_first(lk, type, member) ({                       \
    list_lk_t   *__lk  = (lk);                                       \
    irqflags_t   __f;                                                \
    spin_lock_irqsave(&__lk->lock, &__f);                            \
    list_head_t *__n   = read_once(__lk->head.next);                 \
    type        *__ret = nullptr;                                    \
    if (__n != &__lk->head) {                                        \
        list_del(__n);                                               \
        __ret = list_entry(__n, type, member);                       \
    }                                                                \
    spin_unlock_irqrestore(&__lk->lock, __f);                        \
    __ret;                                                           \
})

/// @brief Atomically pop the last entry; returns nullptr if empty.
///        Acquires @lk->lock.  O(1).
#define list_lk_pop_last(lk, type, member) ({                        \
    list_lk_t   *__lk  = (lk);                                       \
    irqflags_t   __f;                                                \
    spin_lock_irqsave(&__lk->lock, &__f);                            \
    list_head_t *__p   = read_once(__lk->head.prev);                 \
    type        *__ret = nullptr;                                    \
    if (__p != &__lk->head) {                                        \
        list_del(__p);                                               \
        __ret = list_entry(__p, type, member);                       \
    }                                                                \
    spin_unlock_irqrestore(&__lk->lock, __f);                        \
    __ret;                                                           \
})

typedef struct {
    list_head_t head;    ///< Sentinel node.
    atomic_t    count;   ///< Number of entries currently in the list.
} list_counted_t;

#define LIST_COUNTED_INIT(name) \
    { .head = LIST_HEAD_INIT((name).head), .count = ATOMIC_INIT(0) }

#define DEFINE_LIST_COUNTED(name)  list_counted_t name = LIST_COUNTED_INIT(name)

/// @brief Initialize a list_counted_t at runtime.
static inline void list_counted_init(list_counted_t *lc) {
    list_init(&lc->head);
    atomic_set(&lc->count, 0);
}

/// @brief Insert @n at the head; atomically increment count.
///        Caller must hold the appropriate external lock for SMP safety.
static inline void list_counted_add(list_head_t *n, list_counted_t *lc) {
    list_add(n, &lc->head);
    atomic_inc(&lc->count);
}

/// @brief Insert @n at the tail; atomically increment count.
static inline void list_counted_add_tail(list_head_t *n, list_counted_t *lc) {
    list_add_tail(n, &lc->head);
    atomic_inc(&lc->count);
}

/// @brief Remove @n; atomically decrement count.  Poisons @n's pointers.
///        Caller must hold the appropriate external lock.
static inline void list_counted_del(list_head_t *n, list_counted_t *lc) {
    list_del(n);
    atomic_dec(&lc->count);
}

/// @brief Remove @n and reinitialize to empty; atomically decrement count.
static inline void list_counted_del_init(list_head_t *n, list_counted_t *lc) {
    list_del_init(n);
    atomic_dec(&lc->count);
}

/// @brief Move @n from @src to the tail of @dst; update both counts.
///        Caller must hold the external lock covering both lists.
static inline void list_counted_move_tail(list_head_t *n,
                                           list_counted_t *src,
                                           list_counted_t *dst) {
    list_del(n);
    atomic_dec(&src->count);
    list_add_tail(n, &dst->head);
    atomic_inc(&dst->count);
}

/// @brief Splice all entries of @src into @dst tail; reinitialize @src.
///        dst->count is incremented by the snapshot of src->count.
///        Caller must hold the external lock covering both lists.
static inline void list_counted_splice_tail_init(list_counted_t *src,
                                                  list_counted_t *dst) {
    if (!list_empty(&src->head)) {
        int32_t n = atomic_read(&src->count);
        __list_splice(&src->head, read_once(dst->head.prev), &dst->head);
        list_init(&src->head);
        atomic_set(&src->count, 0);
        atomic_add_return(&dst->count, n);
    }
}

/// @brief Return the current entry count.
///        The result is a snapshot; a concurrent mutation may change it
///        immediately after the return.
static inline int32_t list_counted_count(const list_counted_t *lc) {
    return atomic_read((atomic_t *)&lc->count);
}

/// @brief True if the list has no entries.
static inline bool list_counted_empty(const list_counted_t *lc) {
    return list_empty(&lc->head);
}

/// @brief True if the list has exactly one entry.
static inline bool list_counted_is_singular(const list_counted_t *lc) {
    return list_is_singular(&lc->head);
}

/// @brief Atomically pop the first entry; decrement count.
///        Returns nullptr if empty.  Caller must hold the appropriate lock.
#define list_counted_pop_first(lc, type, member) ({                    \
    list_counted_t *__lc  = (lc);                                      \
    list_head_t    *__n   = read_once(__lc->head.next);                \
    type           *__ret = nullptr;                                   \
    if (__n != &__lc->head) {                                          \
        list_del(__n);                                                 \
        atomic_dec(&__lc->count);                                      \
        __ret = list_entry(__n, type, member);                         \
    }                                                                  \
    __ret;                                                             \
})

/// @brief Atomically pop the last entry; decrement count.
///        Returns nullptr if empty.  Caller must hold the appropriate lock.
#define list_counted_pop_last(lc, type, member) ({                     \
    list_counted_t *__lc  = (lc);                                      \
    list_head_t    *__p   = read_once(__lc->head.prev);                \
    type           *__ret = nullptr;                                   \
    if (__p != &__lc->head) {                                          \
        list_del(__p);                                                 \
        atomic_dec(&__lc->count);                                      \
        __ret = list_entry(__p, type, member);                         \
    }                                                                  \
    __ret;                                                             \
})
