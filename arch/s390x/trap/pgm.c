// SPDX-License-Identifier: Apache-2.0
// arch/s390x/trap/pgm.c
//
/// @brief Program-check (exception) handler for z/Architecture.

#include <arch/s390x/cpu/irq_frame.h>
#include <arch/s390x/cpu/lowcore.h>
#include <zxfoundation/sys/irq/irqdesc.h>

/// @brief Program-check C handler — called from trap_pgm_entry.
/// @param frame  Interrupt frame built by entry.S on the async stack.
void do_pgm_check(zx_irq_frame_t *frame) {
    zx_lowcore_t *lc = zx_lowcore();

    const uint16_t pgm_code = lc->pgm_code & 0x7FFFU;
    const uint16_t irq      = (uint16_t)(ZX_IRQ_BASE_PGM + pgm_code);

    irq_dispatch(irq, frame);
}
