/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/init/zxfl/common/psw.c
/// @brief Bootloader-private PSW initialization.

#include <arch/s390x/init/zxfl/psw.h>

/// @brief Install disabled-wait PSWs into all six new PSW slots.
void zxfl_psw_install_safe_psws(void) {
    arch_psw_set_raw(PSW_LC_RESTART,  PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1A0ULL);
    arch_psw_set_raw(PSW_LC_EXTERNAL, PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1B0ULL);
    arch_psw_set_raw(PSW_LC_SVC,      PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1C0ULL);
    arch_psw_set_raw(PSW_LC_PROGRAM,  PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1D0ULL);
    arch_psw_set_raw(PSW_LC_MCCK,     PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1E0ULL);
    arch_psw_set_raw(PSW_LC_IO,       PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1F0ULL);
}
