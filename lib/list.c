// SPDX-License-Identifier: Apache-2.0
// lib/list.c
//
/// @brief list validation code

#include <lib/list.h>
#include <zxfoundation/sys/syschk.h>

static inline bool check_data_corruption(bool v) { return v; }
#define CHECK_DATA_CORRUPTION(condition, addr, fmt, ...)	              	 \
        check_data_corruption(({				                         	 \
            bool corruption = unlikely(condition);	             		     \
            if (corruption) {				                               	 \
                zx_system_check(ZX_SYSCHK_CORE_CORRUPT, fmt, ##__VA_ARGS__); \
            }				                                            	 \
        corruption;		                                     			     \
    }))

bool __list_add_valid_or_report(list_head_t *new,
                                list_head_t *prev,
                                list_head_t *next) {
    if (CHECK_DATA_CORRUPTION(prev == nullptr, nullptr,
                              "lib: list_add corruption. prev is nullptr.\n") ||
        CHECK_DATA_CORRUPTION(next == nullptr, nullptr,
                              "lib: list_add corruption. next is nullptr.\n") ||
        CHECK_DATA_CORRUPTION(next->prev != prev, next,
                              "lib: list_add corruption. next->prev should be prev "
                              "(%px), but was %px. (next=%px).\n",
                              prev, next->prev, next) ||
        CHECK_DATA_CORRUPTION(prev->next != next, prev,
                              "lib: list_add corruption. prev->next should be next "
                              "(%px), but was %px. (prev=%px).\n",
                              next, prev->next, prev) ||
        CHECK_DATA_CORRUPTION(
            new == prev || new == next, nullptr,
            "lib: list_add double add: new=%px, prev=%px, next=%px.\n", new, prev,
            next))
        return false;

    return true;
}

bool __list_del_entry_valid_or_report(list_head_t *entry) {
    list_head_t *prev = entry->prev;
    list_head_t *next = entry->next;

    if (CHECK_DATA_CORRUPTION(next == nullptr, nullptr,
                              "lib: list_del corruption, %px->next is nullptr\n",
                              entry) ||
        CHECK_DATA_CORRUPTION(prev == nullptr, nullptr,
                              "lib: list_del corruption, %px->prev is nullptr\n",
                              entry) ||
        CHECK_DATA_CORRUPTION(prev->next != entry, prev,
                              "lib: list_del corruption. prev->next should be %px, "
                              "but was %px. (prev=%px)\n",
                              entry, prev->next, prev) ||
        CHECK_DATA_CORRUPTION(next->prev != entry, next,
                              "lib: list_del corruption. next->prev should be %px, "
                              "but was %px. (next=%px)\n",
                              entry, next->prev, next))
        return false;

    return true;
}
