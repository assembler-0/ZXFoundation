// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/psw.c
//
/// @brief Unified PSW manager — runtime implementation.

#include <arch/s390x/cpu/psw.h>

void psw_install_new_psws(void) {
    // Each slot gets PSW_MASK_DISABLED_WAIT with a unique sentinel address.
    // The sentinel encodes the interrupt class in the low byte so that an
    // operator can identify which unexpected interrupt fired from the PSW
    // displayed on the hardware console.
    psw_set_raw(PSW_LC_RESTART,  PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1A0ULL);
    psw_set_raw(PSW_LC_EXTERNAL, PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1B0ULL);
    psw_set_raw(PSW_LC_SVC,      PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1C0ULL);
    psw_set_raw(PSW_LC_PROGRAM,  PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1D0ULL);
    psw_set_raw(PSW_LC_MCCK,     PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1E0ULL);
    psw_set_raw(PSW_LC_IO,       PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1F0ULL);
}
