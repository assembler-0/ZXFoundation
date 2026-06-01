/// SPDX-License-Identifier: Apache-2.0
/// @file timer.c
/// @brief Per-CPU hierarchical timer wheel — 8 levels × 64 slots.

#include <arch/s390x/cpu/processor.h>
#include <zxfoundation/time/timer.h>
#include <arch/s390x/time/tod.h>
#include <zxfoundation/percpu.h>
#include <lib/list.h>
#include <zxfoundation/types.h>

/// @brief One wheel per CPU, stored in BSS (zero-initialized).
static timer_wheel_t s_wheels[MAX_CPUS];

/// @brief Slot width (in TOD ticks) of each level.
static constexpr uint64_t s_slot_width[TIMER_WHEEL_LEVELS] = {
    TOD_1MS_IN_TOD,                         // level 0: 1 ms
    TOD_1MS_IN_TOD  << 6,                   // level 1: 64 ms
    TOD_1MS_IN_TOD  << 12,                  // level 2: ~4 s
    TOD_1MS_IN_TOD  << 18,                  // level 3: ~4 min
    TOD_1MS_IN_TOD  << 24,                  // level 4: ~4.5 h
    TOD_1MS_IN_TOD  << 30,                  // level 5: ~12 d
    TOD_1MS_IN_TOD  << 36,                  // level 6: ~2 y
    TOD_1MS_IN_TOD  << 42,                  // level 7: ~140 y
};

/// @brief Return the range spanned by one slot at level @p l.
static inline uint64_t level_range(unsigned int l) {
    return s_slot_width[l] * TIMER_WHEEL_SLOTS;
}

///  @brief Initialize the timer wheel for the current CPU.
void timer_wheel_init(void) {
    int cpu = arch_smp_processor_id();
    timer_wheel_t *w = &s_wheels[cpu];

    for (unsigned int l = 0; l < TIMER_WHEEL_LEVELS; l++) {
        for (unsigned int s = 0; s < TIMER_WHEEL_SLOTS; s++)
            list_init(&w->slots[l][s]);
        w->slot_idx[l] = 0;
    }
    w->current_tod = tod_read();
}

/// @brief Determine the level and slot index for a given expiration time.
/// @param[in]  w        timer wheel
/// @param[in]  expires  absolute expiration time
/// @param[in]  now      current time
/// @param[out] out_level level index
/// @param[out] out_slot  slot index
static void wheel_place(const timer_wheel_t *w, uint64_t expires, uint64_t now,
                        unsigned int *out_level, unsigned int *out_slot) {
    uint64_t delta = (expires > now) ? (expires - now) : 0;

    for (unsigned int l = 0; l < TIMER_WHEEL_LEVELS - 1; l++) {
        if (delta < level_range(l)) {
            uint64_t offset = delta / s_slot_width[l] + 1;
            *out_level = l;
            *out_slot  = (unsigned int)((w->slot_idx[l] + offset) % TIMER_WHEEL_SLOTS);
            return;
        }
    }
    *out_level = TIMER_WHEEL_LEVELS - 1;
    *out_slot  = (unsigned int)(w->slot_idx[TIMER_WHEEL_LEVELS - 1]);
}

///  @brief Add timer to the wheel
///  @param[in] t timer to add
void timer_add(timer_t *t) {
    int cpu = arch_smp_processor_id();
    timer_wheel_t *w = &s_wheels[cpu];

    if (t->pending)
        return; // already in the wheel; caller must cancel first

    unsigned int level, slot;
    wheel_place(w, t->expires, w->current_tod, &level, &slot);

    list_add_tail(&t->node, &w->slots[level][slot]);
    t->pending = 1;
}

/// @brief Cancel timer
/// @param[in] t timer to cancel
void timer_cancel(timer_t *t) {
    if (!t->pending)
        return;
    list_del(&t->node);
    list_init(&t->node);
    t->pending = 0;
}

/// @brief Cascade timers from level @p l into lower levels.
///        Called when the slot pointer of level l wraps around (full revolution).
static void cascade(timer_wheel_t *w, unsigned int l) {
    unsigned int slot = (unsigned int)(w->slot_idx[l]);
    list_head_t *head = &w->slots[l][slot];

    while (!list_empty(head)) {
        timer_t *t = list_entry(head->next, timer_t, node);
        list_del(&t->node);
        list_init(&t->node);
        t->pending = 0;
        timer_add(t);
    }
}

/// @brief Advance timer wheel to new time
/// @param[in] now  new time (TOD)
void timer_wheel_advance(uint64_t now) {
    int cpu = arch_smp_processor_id();
    timer_wheel_t *w = &s_wheels[cpu];

    while (w->current_tod < now) {
        w->current_tod += s_slot_width[0];

        unsigned int s0 = (unsigned int)(w->slot_idx[0]);
        list_head_t *head = &w->slots[0][s0];

        while (!list_empty(head)) {
            timer_t *t = list_entry(head->next, timer_t, node);
            list_del(&t->node);
            list_init(&t->node);
            t->pending = 0;
            if (t->fn)
                t->fn(t->data);
        }

        w->slot_idx[0] = (w->slot_idx[0] + 1) % TIMER_WHEEL_SLOTS;

        if (w->slot_idx[0] == 0) {
            for (unsigned int l = 1; l < TIMER_WHEEL_LEVELS; l++) {
                cascade(w, l);
                w->slot_idx[l] = (w->slot_idx[l] + 1) % TIMER_WHEEL_SLOTS;
                if (w->slot_idx[l] != 0)
                    break;
            }
        }
    }
}

/// @brief Return the time of the earliest pending timer.
/// @return time of earliest pending timer, or UINT64_MAX if none
uint64_t timer_wheel_next_expiry(void) {
    int cpu = arch_smp_processor_id();
    timer_wheel_t *w = &s_wheels[cpu];
    uint64_t earliest = UINT64_MAX;

    for (unsigned int l = 0; l < TIMER_WHEEL_LEVELS; l++) {
        for (unsigned int s = 0; s < TIMER_WHEEL_SLOTS; s++) {
            if (list_empty(&w->slots[l][s]))
                continue;
            uint64_t slot_base = w->current_tod +
                ((s - w->slot_idx[l] + TIMER_WHEEL_SLOTS) % TIMER_WHEEL_SLOTS)
                * s_slot_width[l];
            if (slot_base < earliest)
                earliest = slot_base;
        }
    }
    return earliest;
}
