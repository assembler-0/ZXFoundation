// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/psw.c
//
/// @brief Unified PSW manager — runtime implementation.

#include <arch/s390x/cpu/psw.h>

void psw_install_new_psws(void) {
    arch_psw_set_raw(PSW_LC_RESTART,  PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1A0ULL);
    arch_psw_set_raw(PSW_LC_EXTERNAL, PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1B0ULL);
    arch_psw_set_raw(PSW_LC_SVC,      PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1C0ULL);
    arch_psw_set_raw(PSW_LC_PROGRAM,  PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1D0ULL);
    arch_psw_set_raw(PSW_LC_MCCK,     PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1E0ULL);
    arch_psw_set_raw(PSW_LC_IO,       PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1F0ULL);
}
