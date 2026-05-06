// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/lowcore.c

#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/cpu/psw.h>

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
///        Overwrites the disabled-wait sentinels with the real kernel handler
///        PSWs.  At this point zx_lowcore() is valid (HHDM is active).
///        The restart PSW is left as disabled-wait until smp_init() writes
///        the AP entry address.
void zx_lowcore_setup_late(void) {
    zx_lowcore_t *lc = zx_lowcore();

    lc->external_new_psw.mask  = PSW_MASK_DISABLED_WAIT;
    lc->external_new_psw.addr  = 0x000000000DEAD1B0ULL;

    lc->svc_new_psw.mask       = PSW_MASK_DISABLED_WAIT;
    lc->svc_new_psw.addr       = 0x000000000DEAD1C0ULL;

    lc->program_new_psw.mask   = PSW_MASK_DISABLED_WAIT;
    lc->program_new_psw.addr   = 0x000000000DEAD1D0ULL;

    lc->mcck_new_psw.mask      = PSW_MASK_DISABLED_WAIT;
    lc->mcck_new_psw.addr      = 0x000000000DEAD1E0ULL;

    lc->io_new_psw.mask        = PSW_MASK_DISABLED_WAIT;
    lc->io_new_psw.addr        = 0x000000000DEAD1F0ULL;

    lc->restart_psw.mask       = PSW_MASK_DISABLED_WAIT;
    lc->restart_psw.addr       = 0x000000000DEAD1A0ULL;
}
