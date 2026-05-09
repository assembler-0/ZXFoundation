// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sys/irq/irqdesc.h
//
/// @brief Generic IRQ descriptor — KOMS-managed.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/object/koms.h>
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
typedef void (*irq_handler_t)(uint16_t irq, zx_irq_frame_t *frame, void *data);

/// @brief IRQ descriptor — KOMS-managed, statically allocated.
///
///        kobject_t is the first member so kobject_container() works and
///        koms_ns_find_get("irq", name) returns a directly castable pointer.
///
///        obj->lock (embedded in kobject_t) serializes handler registration
///        and unregistration, replacing the previous unsynchronized access.
///
///        The dispatch hot path bypasses KOMS entirely — it indexes irq_table[]
///        directly and reads handler/data under irqsave, paying zero kobject
///        overhead per interrupt.
typedef struct irq_desc {
    kobject_t       obj;        ///< KOMS base — must be first.
    irq_handler_t   handler;
    void           *data;
    uint32_t        irq_flags;  ///< ZX_IRQF_* (distinct from obj.flags).
    uint32_t        count;      ///< Dispatch count (incremented in hot path).
    uint16_t        irq_nr;     ///< Hardware IRQ number.
} irq_desc_t;

/// @brief Initialize the IRQ subsystem and register all descriptors with KOMS.
///        Must be called after koms_init().
void irq_subsystem_init(void);

/// @brief Register a handler.
/// @return 0 on success, -1 on range error or conflict.
int irq_register(uint16_t irq, irq_handler_t handler, void *data, uint32_t flags);

/// @brief Unregister the handler for an IRQ.
void irq_unregister(uint16_t irq);

/// @brief Dispatch an interrupt.  Hot path — no KOMS overhead.
void irq_dispatch(uint16_t irq, zx_irq_frame_t *frame);

/// @brief Look up a descriptor by IRQ number via KOMS namespace.
///        Acquires a reference; caller must koms_put() when done.
/// @return Referenced irq_desc_t *, or nullptr.
irq_desc_t *irq_get_desc(uint16_t irq);
