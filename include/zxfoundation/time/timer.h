// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/time/timer.h
//
/// @brief Per-CPU hierarchical timer wheel.

#pragma once

#include <zxfoundation/types.h>
#include <lib/list.h>

/// Number of levels in the timer wheel.
#define TIMER_WHEEL_LEVELS      8U
/// Number of slots per level.
#define TIMER_WHEEL_SLOTS       64U

/// @brief Timer callback function.
/// @param data  Opaque pointer passed to timer_add().
typedef void (*timer_fn_t)(void *data);

/// @brief Timer descriptor.  Embed in the owning structure or allocate
///        from a slab cache.  Do not copy after timer_add().
typedef struct timer {
    list_node_t     node;       ///< Linkage in a wheel slot list.
    uint64_t        expires;    ///< Absolute expiry in TOD units.
    timer_fn_t      fn;         ///< Callback — called in hard-IRQ context.
    void           *data;       ///< Opaque argument to fn.
    uint8_t         pending;    ///< 1 if currently in a wheel slot.
    uint8_t         _pad[7];
} timer_t;

/// @brief Per-CPU timer wheel state.
typedef struct timer_wheel {
    list_node_t     slots[TIMER_WHEEL_LEVELS][TIMER_WHEEL_SLOTS];
    uint64_t        current_tod;    ///< TOD value of the last advance.
    uint64_t        slot_idx[TIMER_WHEEL_LEVELS]; ///< Current slot per level.
} timer_wheel_t;

/// @brief Initialize the per-CPU timer wheel.
///        Called once per CPU from time_init() / time_init_ap().
void timer_wheel_init(void);

/// @brief Add a timer to the current CPU's wheel.
/// @param t  Timer to add.  Must not already be pending.
void timer_add(timer_t *t);

/// @brief Cancel a pending timer.
/// @param t  Timer to cancel.
void timer_cancel(timer_t *t);

/// @brief Advance the current CPU's timer wheel to @p now.
/// @param now  Current TOD clock value.
void timer_wheel_advance(uint64_t now);

/// @brief Return the TOD value of the next pending timer on this CPU,
///        or UINT64_MAX if the wheel is empty.
uint64_t timer_wheel_next_expiry(void);
