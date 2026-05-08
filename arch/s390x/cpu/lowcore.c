// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/lowcore.c

#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/cpu/psw.h>

extern void trap_pgm_entry(void);
extern void trap_ext_entry(void);
extern void trap_io_entry(void);
extern void trap_mcck_entry(void);

/// @brief Install the four interrupt handler new PSWs into any lowcore.
///
///        Called for the BSP lowcore by zx_lowcore_setup_late(), and for
///        each AP lowcore by smp_init() before SIGP RESTART.  The hardware
///        reads the new PSW from the *executing CPU's* lowcore (selected by
///        the prefix register), so every CPU needs its own copy.
void lc_install_handler_psws(zx_lowcore_t *lc) {
    lc->program_new_psw.mask  = PSW_MASK_KERNEL_DAT;
    lc->program_new_psw.addr  = (uint64_t)(uintptr_t)trap_pgm_entry;

    lc->external_new_psw.mask = PSW_MASK_KERNEL_DAT;
    lc->external_new_psw.addr = (uint64_t)(uintptr_t)trap_ext_entry;

    lc->io_new_psw.mask       = PSW_MASK_KERNEL_DAT;
    lc->io_new_psw.addr       = (uint64_t)(uintptr_t)trap_io_entry;

    lc->mcck_new_psw.mask     = PSW_MASK_KERNEL_DAT;
    lc->mcck_new_psw.addr     = (uint64_t)(uintptr_t)trap_mcck_entry;

    // TODO: syscall
    lc->svc_new_psw.mask      = PSW_MASK_DISABLED_WAIT;
    lc->svc_new_psw.addr      = 0x000000000DEAD1C0ULL;
}

void zx_lowcore_setup_early(void) {
    psw_install_new_psws();
}

void zx_lowcore_setup_late(void) {
    zx_lowcore_t *lc = zx_lowcore();

    lc_install_handler_psws(lc);

    lc->restart_psw.mask = PSW_MASK_DISABLED_WAIT;
    lc->restart_psw.addr = CONFIG_PANIC_HALT_ADDR;
}
