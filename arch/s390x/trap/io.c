// SPDX-License-Identifier: Apache-2.0
// arch/s390x/trap/io.c
//
/// @brief I/O interrupt handler for z/Architecture.

#include <arch/s390x/cpu/irq_frame.h>
#include <arch/s390x/cpu/lowcore.h>
#include <zxfoundation/sys/irq/irqdesc.h>

/// @brief I/O interrupt C handler — called from trap_io_entry.
/// @param frame  Interrupt frame built by entry.S on the async stack.
void do_io_interrupt(zx_irq_frame_t *frame) {
    zx_lowcore_t *lc = zx_lowcore();

    const uint16_t sch_nr = lc->subchannel_nr;
    const uint16_t irq    = (uint16_t)(ZX_IRQ_BASE_IO + (sch_nr & 0x00FFU));

    irq_dispatch(irq, frame);
}
