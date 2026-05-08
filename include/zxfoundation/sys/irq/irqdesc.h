// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sys/irq/irqdesc.h
//
/// @brief Generic IRQ descriptor table.

#pragma once

#include <zxfoundation/types.h>
#include <arch/s390x/cpu/irq_frame.h>

/// Total number of IRQ descriptor slots.
#define ZX_IRQ_NR_MAX       0x0400U

/// IRQ number base offsets per class.
#define ZX_IRQ_BASE_PGM     0x0000U
#define ZX_IRQ_BASE_EXT     0x0100U
#define ZX_IRQ_BASE_IO      0x0200U
#define ZX_IRQ_BASE_MCCK    0x0300U

/// IRQ descriptor flags.
#define ZX_IRQF_SHARED      (1U << 0)   ///< Multiple handlers may share this IRQ.
#define ZX_IRQF_DISABLED    (1U << 1)   ///< Descriptor registered but masked.

/// @brief IRQ handler function signature.
/// @param irq    IRQ number (ZX_IRQ_BASE_* + hardware code).
/// @param frame  Pointer to the interrupt frame saved by entry.S.
/// @param data   Opaque pointer registered with the descriptor.
typedef void (*irq_handler_t)(uint16_t irq, zx_irq_frame_t *frame, void *data);

/// @brief IRQ descriptor — one entry in the global descriptor table.
typedef struct {
    irq_handler_t   handler;    ///< Registered handler, or NULL for default.
    void           *data;       ///< Opaque argument passed to handler.
    uint32_t        flags;      ///< ZX_IRQF_* flags.
    uint32_t        count;      ///< Number of times this IRQ has fired.
} irq_desc_t;

/// @brief Register a handler for a specific IRQ number.
///
/// @param irq      IRQ number (ZX_IRQ_BASE_* + hardware code).
/// @param handler  Handler function.
/// @param data     Opaque argument forwarded to handler on each invocation.
/// @param flags    ZX_IRQF_* flags.
/// @return 0 on success, -1 if irq >= ZX_IRQ_NR_MAX or slot already taken
///         without ZX_IRQF_SHARED.
int irq_register(uint16_t irq, irq_handler_t handler, void *data, uint32_t flags);

/// @brief Unregister the handler for a specific IRQ number.
/// @param irq  IRQ number.
void irq_unregister(uint16_t irq);

/// @brief Dispatch an interrupt to its registered handler.
/// @param irq    IRQ number.
/// @param frame  Interrupt frame pointer.
void irq_dispatch(uint16_t irq, zx_irq_frame_t *frame);

/// @brief Return a read-only pointer to a descriptor (for diagnostics).
/// @param irq  IRQ number.
/// @return Pointer to the descriptor, or NULL if irq >= ZX_IRQ_NR_MAX.
const irq_desc_t *irq_get_desc(uint16_t irq);
