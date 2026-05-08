// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/lowcore.c

#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/cpu/psw.h>

/*
 * Forward declarations for the entry stubs defined in arch/s390x/trap/entry.S.
 * These are virtual addresses valid after DAT is enabled.
 */
extern void trap_pgm_entry(void);
extern void trap_ext_entry(void);
extern void trap_io_entry(void);
extern void trap_mcck_entry(void);

/// @brief Early lowcore setup — called from stage2/entry.S before DAT is on.
///
///        Installs disabled-wait PSWs into all six new PSW slots using the
///        correct PoP-defined offsets (0x1A0–0x1F0).  This replaces the
///        previous implementation that incorrectly wrote to the interrupt
///        parameter area (0x080–0x0C0).
void zx_lowcore_setup_early(void) {
    psw_install_new_psws();
}

/// @brief Late lowcore setup — called from the kernel after DAT is on.
///
///        Overwrites the disabled-wait sentinels installed by setup_early
///        with the real kernel handler PSWs.  At this point zx_lowcore()
///        is valid (HHDM is active) and the entry stub virtual addresses
///        are reachable.
///
///        PSW mask for all handlers:
///          PSW_MASK_KERNEL_DAT — supervisor mode, DAT on, all interrupts
///          disabled.  Handlers re-enable specific interrupt classes as
///          needed via STOSM.
///
///        The restart PSW is left as disabled-wait until smp_init() writes
///        the AP entry address.  The SVC PSW remains disabled-wait until
///        the syscall layer is implemented.
void zx_lowcore_setup_late(void) {
    zx_lowcore_t *lc = zx_lowcore();

    lc->program_new_psw.mask  = PSW_MASK_KERNEL_DAT;
    lc->program_new_psw.addr  = (uint64_t)(uintptr_t)trap_pgm_entry;

    lc->external_new_psw.mask = PSW_MASK_KERNEL_DAT;
    lc->external_new_psw.addr = (uint64_t)(uintptr_t)trap_ext_entry;

    lc->io_new_psw.mask       = PSW_MASK_KERNEL_DAT;
    lc->io_new_psw.addr       = (uint64_t)(uintptr_t)trap_io_entry;

    lc->mcck_new_psw.mask     = PSW_MASK_KERNEL_DAT;
    lc->mcck_new_psw.addr     = (uint64_t)(uintptr_t)trap_mcck_entry;

    /* SVC — disabled-wait until syscall layer is implemented. */
    lc->svc_new_psw.mask      = PSW_MASK_DISABLED_WAIT;
    lc->svc_new_psw.addr      = 0x000000000DEAD1C0ULL;

    /* Restart — BSP restart PSW is unused; AP restart PSWs are written
     * per-lowcore by smp_init() via lc_set_restart_psw(). */
    lc->restart_psw.mask      = PSW_MASK_DISABLED_WAIT;
    lc->restart_psw.addr      = 0x000000000DEAD1A0ULL;
}
