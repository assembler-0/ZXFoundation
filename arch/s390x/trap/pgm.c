// SPDX-License-Identifier: Apache-2.0
// arch/s390x/trap/pgm.c
//
/// @brief Program-check (exception) handler for z/Architecture.

#include <arch/s390x/cpu/irq_frame.h>
#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/mmu/mmu.h>
#include <zxfoundation/sys/irq/irqdesc.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/sys/printk.h>

static void do_unhandled_trap(const char *msg, uint16_t code, zx_irq_frame_t *frame) {
    zx_lowcore_t *lc = zx_lowcore();
    zx_system_check(ZX_SYSCHK_ARCH_UNHANDLED_TRAP,
                    "irq: %s 0x%04x at PSW 0x%016llx (TEA=0x%016llx ILC=%u)\n",
                    msg, code, (unsigned long long)frame->psw_addr,
                    (unsigned long long)lc->trans_exc_code,
                    (unsigned)(lc->pgm_ilc >> 1));
}

static void pgm_seg_fault_handler(uint16_t irq, zx_irq_frame_t *frame, void *data) {
    (void)irq; (void)data;
    do_unhandled_trap("segment translation exception", 0x0010, frame);
}

/// @brief Program-check C handler — called from trap_pgm_entry.
/// @param frame  Interrupt frame built by entry.S on the async stack.
void do_pgm_check(zx_irq_frame_t *frame) {
    zx_lowcore_t *lc = zx_lowcore();

    const uint16_t pgm_code = lc->pgm_code & 0x7FFFU;
    const uint16_t irq      = (uint16_t)(ZX_IRQ_BASE_PGM + pgm_code);

    if (pgm_code == 0x0010) {
        pgm_seg_fault_handler(irq, frame, nullptr);
        return;
    }

    irq_dispatch(irq, frame);
}
