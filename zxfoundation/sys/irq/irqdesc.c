// SPDX-License-Identifier: Apache-2.0
// zxfoundation/irq/irqdesc.c
//
/// @brief Generic IRQ descriptor table implementation.
///
///        The table is a flat array of ZX_IRQ_NR_MAX (0x400) entries in BSS.
///        All slots are zero-initialized; a NULL handler field means "use the
///        default handler".
///
///        The default handler distinguishes fatal from non-fatal IRQ classes:
///
///          PGM (0x0000–0x00FF)  — always fatal (unhandled kernel exception).
///          EXT (0x0100–0x01FF)  — logged and silently dropped.
///          IO  (0x0200–0x02FF)  — logged and silently dropped.
///          MCCK(0x0300–0x03FF)  — always fatal (unhandled machine check).
///
///        irq_register() and irq_unregister() are not thread-safe at this
///        stage.  They must be called during single-threaded init or with
///        external serialization.  A spinlock will be added when the
///        scheduler is live.

#include <zxfoundation/sys/irq/irqdesc.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>

/// The global descriptor table.  Zero-initialized by BSS.
static irq_desc_t irq_table[ZX_IRQ_NR_MAX];

/// @brief Default handler for unregistered IRQs.
///
///        PGM and MCCK classes are fatal — an unhandled exception or machine
///        check in kernel mode cannot be recovered from.  EXT and IO are
///        logged and dropped; they may arrive before drivers register.
static void irq_default_handler(uint16_t irq, zx_irq_frame_t *frame, void *data) {
    (void)data;

    if (irq < ZX_IRQ_BASE_EXT) {
        zx_system_check(ZX_SYSCHK_ARCH_UNHANDLED_TRAP,
                        "irq: unhandled program check 0x%04x at PSW 0x%016llx",
                        (unsigned)irq,
                        (unsigned long long)frame->psw_addr);
    }

    if (irq >= ZX_IRQ_BASE_MCCK) {
        zx_system_check(ZX_SYSCHK_ARCH_MCHECK,
                        "irq: unhandled machine check sub-code 0x%04x",
                        (unsigned)(irq - ZX_IRQ_BASE_MCCK));
    }

    printk("irq: unhandled irq 0x%04x (psw=0x%016llx) — dropped\n",
           (unsigned)irq,
           (unsigned long long)frame->psw_addr);
}

int irq_register(uint16_t irq, irq_handler_t handler, void *data, uint32_t flags) {
    if (irq >= ZX_IRQ_NR_MAX)
        return -1;

    irq_desc_t *desc = &irq_table[irq];

    if (desc->handler && !(flags & ZX_IRQF_SHARED))
        return -1;

    desc->handler = handler;
    desc->data    = data;
    desc->flags   = flags;
    return 0;
}

void irq_unregister(uint16_t irq) {
    if (irq >= ZX_IRQ_NR_MAX)
        return;

    irq_desc_t *desc = &irq_table[irq];
    desc->handler = nullptr;
    desc->data    = nullptr;
    desc->flags   = 0;
}

void irq_dispatch(uint16_t irq, zx_irq_frame_t *frame) {
    if (irq >= ZX_IRQ_NR_MAX) {
        zx_system_check(ZX_SYSCHK_ARCH_UNHANDLED_TRAP,
                        "irq: dispatch out of range irq=0x%04x", (unsigned)irq);
    }

    irq_desc_t *desc = &irq_table[irq];

    desc->count++;

    irq_handler_t h = desc->handler;
    if (!h)
        h = irq_default_handler;

    h(irq, frame, desc->data);
}

const irq_desc_t *irq_get_desc(uint16_t irq) {
    if (irq >= ZX_IRQ_NR_MAX)
        return nullptr;
    return &irq_table[irq];
}
