/// SPDX-License-Identifier: Apache-2.0
/// @file irqdesc.h
/// @brief Generic IRQ descriptor — KOMS-managed.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/object/koms.h>
#include <arch/s390x/cpu/irq_frame.h>

/// @brief Total number of IRQ descriptor slots.
#define ZX_IRQ_NR_MAX       0x0400U

/// @name IRQ number base offsets per class.
/// @{
#define ZX_IRQ_BASE_PGM     0x0000U
#define ZX_IRQ_BASE_EXT     0x0100U
#define ZX_IRQ_BASE_IO      0x0200U
#define ZX_IRQ_BASE_MCCK    0x0300U
/// @}

/// @name IRQ descriptor flags.
/// @{
#define ZX_IRQF_SHARED      (1U << 0)   ///< Multiple handlers may share this IRQ.
#define ZX_IRQF_DISABLED    (1U << 1)   ///< Descriptor registered but masked.
/// @}

/// @brief IRQ handler function signature.
typedef void (*irq_handler_t)(uint16_t irq, arch_s390x_irq_frame_t *frame, void *data);

/// @brief IRQ descriptor — KOMS-managed, statically allocated.
typedef struct irq_desc {
    kobject_t       obj;        ///< KOMS base — must be first.
    irq_handler_t   handler;
    void           *data;
    uint32_t        irq_flags;  ///< ZX_IRQF_* (distinct from obj.flags).
    uint32_t        count;      ///< Dispatch count (incremented in hot path).
    uint16_t        irq_nr;     ///< Hardware IRQ number.
} irq_desc_t;

/// @brief Initialize the IRQ subsystem: namespaces, objects, default handlers.
void irq_subsystem_init(void);

/// @brief Register an IRQ handler.
/// @param[in] irq IRQ number
/// @param[in] handler Pointer to handler function
/// @param[in] data Pointer to data to be passed to handler
/// @param[in] flags IRQ registration flags (e.g. ZX_IRQF_SHARED)
/// @return 0 on success, -1 on failure
int irq_register(uint16_t irq, irq_handler_t handler, void *data, uint32_t flags);

/// @brief Unregister an IRQ handler.
/// @param[in] irq IRQ number
void irq_unregister(uint16_t irq);

/// @brief Dispatch an IRQ to its registered handler.
/// @param[in] irq IRQ number
/// @param[in] frame Pointer to saved register state (PSW etc.)
void irq_dispatch(uint16_t irq, arch_s390x_irq_frame_t *frame);

/// @brief Get an IRQ descriptor.
/// @param[in] irq IRQ number
/// @return Pointer to IRQ descriptor, or nullptr if invalid IRQ number.
irq_desc_t *irq_get_desc(uint16_t irq);
