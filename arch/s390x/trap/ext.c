// SPDX-License-Identifier: Apache-2.0
// arch/s390x/trap/ext.c
//
/// @brief External interrupt handler for z/Architecture.

#include <arch/s390x/cpu/irq_frame.h>
#include <arch/s390x/cpu/lowcore.h>
#include <zxfoundation/sys/irq/irqdesc.h>

/// @brief External interrupt C handler — called from trap_ext_entry.
/// @param frame  Interrupt frame built by entry.S on the async stack.
void do_ext_interrupt(zx_irq_frame_t *frame) {
    zx_lowcore_t *lc = zx_lowcore();

    const uint16_t ext_code = lc->ext_int_code;
    const uint16_t irq      = (uint16_t)(ZX_IRQ_BASE_EXT + ext_code);

    irq_dispatch(irq, frame);
}
