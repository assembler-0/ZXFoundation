// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sys/irq/irqdesc.c
//
/// @brief IRQ descriptor table — KOMS-integrated implementation.
///
///        DESIGN
///        ======
///        irq_table[] is a flat static array of irq_desc_t.  Each entry
///        embeds a kobject_t and is registered in the "irq" KOMS namespace
///        at irq_subsystem_init() time.  The kobjects are never freed —
///        they live for the kernel lifetime — so ops->release is a no-op.
///
///        LOCKING
///        =======
///        irq_register() and irq_unregister() acquire desc->obj.lock
///        (irqsave) to serialize handler installation.  This replaces the
///        previous unsynchronized access.
///
///        DISPATCH HOT PATH
///        =================
///        irq_dispatch() indexes irq_table[] directly by IRQ number and
///        reads handler/data under a brief irqsave critical section.  It
///        does not touch the kobject machinery — zero KOMS overhead per
///        interrupt.

#include <zxfoundation/sys/irq/irqdesc.h>
#include <zxfoundation/object/koms.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>
#include <lib/string.h>
#include <lib/vsprintf.h>
#include <zxfoundation/common.h>

static irq_desc_t irq_table[ZX_IRQ_NR_MAX];
static kobj_ns_t  irq_ns;

/// @brief release() is a no-op: irq_table[] is statically allocated.
///        It must exist because kobject_ops_t::release is mandatory.
static void irq_desc_release(kobject_t *obj) { (void)obj; }

static const kobject_ops_t irq_desc_kobj_ops = {
    .release = irq_desc_release,
};

static kobj_type_t irq_desc_type = {
    .type_id  = KOBJ_TYPE_IRQ,
    .name     = "irq_desc",
    .obj_size = sizeof(irq_desc_t),
    .cache    = nullptr,            // static allocation — no slab needed
    .kobj_ops = &irq_desc_kobj_ops,
    .type_ops = nullptr,
};

/// @brief Format an IRQ number into a static per-descriptor name buffer.
///        Format: "pgm-XXXX", "ext-XXXX", "io-XXXX", "mck-XXXX"
///        where XXXX is the lower 8 bits in hex.
#define ZX_IRQ_NAMEBUF_SZ 12
static char irq_name_bufs[ZX_IRQ_NR_MAX][ZX_IRQ_NAMEBUF_SZ];

static void irq_format_name(uint16_t irq, char *buf) {
    const char *prefix;

    if      (irq < ZX_IRQ_BASE_EXT)  prefix = "pgm";
    else if (irq < ZX_IRQ_BASE_IO)   prefix = "ext";
    else if (irq < ZX_IRQ_BASE_MCCK) prefix = "io";
    else                              prefix = "mck";

    snprintf(buf, ZX_IRQ_NAMEBUF_SZ, "%s-%04x", prefix, irq & 0x00FFU);
}

static void irq_default_handler(uint16_t irq, zx_irq_frame_t *frame, void *data) {
    (void)data;

    if (irq < ZX_IRQ_BASE_EXT)
        zx_system_check(ZX_SYSCHK_ARCH_UNHANDLED_TRAP,
                        "irq: unhandled program check 0x%04x at PSW 0x%016llx",
                        (unsigned)irq, (unsigned long long)frame->psw_addr);

    if (irq >= ZX_IRQ_BASE_MCCK)
        zx_system_check(ZX_SYSCHK_ARCH_MCHECK,
                        "irq: unhandled machine check sub-code 0x%04x",
                        (unsigned)(irq - ZX_IRQ_BASE_MCCK));

    printk(ZX_WARN "irq: unhandled irq 0x%04x (psw=0x%016llx) — dropped\n",
           (unsigned)irq, (unsigned long long)frame->psw_addr);
}

void irq_subsystem_init(void) {
    koms_type_register(&irq_desc_type);
    koms_ns_init(&irq_ns, "irq", nullptr, nullptr);

    for (uint16_t i = 0; i < ZX_IRQ_NR_MAX; i++) {
        irq_desc_t *desc = &irq_table[i];
        desc->irq_nr    = i;
        desc->handler   = nullptr;
        desc->data      = nullptr;
        desc->irq_flags = 0;
        desc->count     = 0;

        irq_format_name(i, irq_name_bufs[i]);

        koms_init_obj(&desc->obj, &irq_desc_type, irq_name_bufs[i], nullptr);

        koms_ns_add(&irq_ns, &desc->obj);
    }

    printk(ZX_INFO "irq: namespaces and objects initialized for %u descriptors\n", ZX_IRQ_NR_MAX);
}

int irq_register(uint16_t irq, irq_handler_t handler, void *data, uint32_t flags) {
    if (irq >= ZX_IRQ_NR_MAX)
        return -1;

    irq_desc_t *desc = &irq_table[irq];

    irqflags_t f;
    spin_lock_irqsave(&desc->obj.lock, &f);

    if (desc->handler && !(flags & ZX_IRQF_SHARED)) {
        spin_unlock_irqrestore(&desc->obj.lock, f);
        return -1;
    }

    desc->handler   = handler;
    desc->data      = data;
    desc->irq_flags = flags;

    spin_unlock_irqrestore(&desc->obj.lock, f);

    koms_event_fire(&desc->obj, KOBJ_EVENT_STATE_CHANGE);
    return 0;
}

void irq_unregister(uint16_t irq) {
    if (irq >= ZX_IRQ_NR_MAX)
        return;

    irq_desc_t *desc = &irq_table[irq];

    irqflags_t f;
    spin_lock_irqsave(&desc->obj.lock, &f);
    desc->handler   = nullptr;
    desc->data      = nullptr;
    desc->irq_flags = 0;
    spin_unlock_irqrestore(&desc->obj.lock, f);

    koms_event_fire(&desc->obj, KOBJ_EVENT_STATE_CHANGE);
}

void irq_dispatch(uint16_t irq, zx_irq_frame_t *frame) {
    if (unlikely(irq >= ZX_IRQ_NR_MAX))
        zx_system_check(ZX_SYSCHK_ARCH_UNHANDLED_TRAP,
                        "irq: dispatch out of range irq=0x%04x", (unsigned)irq);

    irq_desc_t *desc = &irq_table[irq];

    irqflags_t f;
    spin_lock_irqsave(&desc->obj.lock, &f);
    irq_handler_t h  = desc->handler;
    void         *d  = desc->data;
    spin_unlock_irqrestore(&desc->obj.lock, f);

    desc->count++;

    if (!h)
        h = irq_default_handler;

    h(irq, frame, d);
}

irq_desc_t *irq_get_desc(uint16_t irq) {
    if (irq >= ZX_IRQ_NR_MAX)
        return nullptr;

    irq_desc_t *desc = &irq_table[irq];
    if (!koms_get_unless_dead(&desc->obj))
        return nullptr;
    return desc;
}
