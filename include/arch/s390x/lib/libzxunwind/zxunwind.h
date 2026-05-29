// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/lib/libzxunwind/zxunwind.h
//
/// @brief Safe, freestanding stack frame unwinder for s390x on ZXFoundation.

#pragma once

#include <zxfoundation/types.h>
#include <arch/s390x/cpu/irq_frame.h>

/// @brief Maximum depth of stack unwinding to prevent infinite loops.
#define ZX_UNWIND_MAX_DEPTH 32

/// @brief Information about a single unwound stack frame.
typedef struct {
    uint64_t pc;                      ///< Program counter (return address/instruction pointer).
    uint64_t sp;                      ///< Stack pointer for this frame.
    char     symbol_name[128];        ///< Resolved symbol name.
    uint64_t symbol_offset;           ///< Offset from the symbol start.
} zx_unwind_frame_t;

/// @brief Safe stack backtrace.
/// @param frames      Output array to store frame information.
/// @param max_depth   Maximum frames to record.
/// @param irq_frame   Optional interrupt/trap frame context. If NULL, unwinds current context.
/// @return Number of frames successfully unwound.
uint32_t zx_unwind_backtrace(zx_unwind_frame_t *frames, uint32_t max_depth,
                             const arch_s390x_irq_frame_t *irq_frame);

typedef void(*zx_unwind_write_cb_t)(const char *msg);

/// @brief Print a backtrace to the simple log / console.
/// @param irq_frame   Optional interrupt/trap frame context.
/// @param cb          Output callback for writing messages.
void zx_unwind_print(const arch_s390x_irq_frame_t *irq_frame, zx_unwind_write_cb_t cb);
