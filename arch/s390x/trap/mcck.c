// SPDX-License-Identifier: Apache-2.0
// arch/s390x/trap/mcck.c
//
/// @brief Machine-check interrupt handler for z/Architecture.

#include <arch/s390x/cpu/irq_frame.h>
#include <arch/s390x/cpu/lowcore.h>
#include <zxfoundation/sys/irq/irqdesc.h>
#include <zxfoundation/sys/syschk.h>

#define MCIC_SYSTEM_DAMAGE  0x8000000000000000ULL

/// @brief Machine-check C handler — called from trap_mcck_entry.
/// @param frame  Interrupt frame built by entry.S on the mcck stack.
void do_mcck_interrupt(zx_irq_frame_t *frame) {
    zx_lowcore_t *lc = zx_lowcore();

    const uint64_t mcic = lc->mcck_interruption_code;

    if (mcic & MCIC_SYSTEM_DAMAGE)
        zx_system_check(ZX_SYSCHK_ARCH_MCHECK, "mcck: system damage — unrecoverable");

    const uint16_t sub_code = (uint16_t)(mcic >> 56);
    const uint16_t irq      = (uint16_t)(ZX_IRQ_BASE_MCCK + sub_code);

    irq_dispatch(irq, frame);
}
