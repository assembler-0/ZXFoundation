// SPDX-License-Identifier: Apache-2.0
// include/lib/list.h
//
/// @brief Generic intrusive doubly linked list.

#pragma once

#include <zxfoundation/types.h>

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

static inline void list_del(list_node_t *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = entry->prev = nullptr;
}

static inline bool list_empty(const list_node_t *head) {
    return head->next == head;
}

#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - (uintptr_t)(&((type *)0)->member)))

#define list_for_each_entry(pos, head, member) \
    for (pos = list_entry((head)->next, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = list_entry(pos->member.next, __typeof__(*pos), member))
