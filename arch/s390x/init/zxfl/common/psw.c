// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/psw.c
//
/// @brief Bootloader-private PSW initialization.
///
///        This file is compiled ONLY into the stage-2 bootloader (zxfl_stage2).
///        It must NOT be linked into the kernel.  The kernel has its own
///        psw_install_new_psws() in arch/s390x/cpu/psw.c.
///
///        Isolation rationale:
///          The bootloader runs in a completely different address space and
///          execution context from the kernel.  Sharing kernel PSW setup code
///          in the bootloader creates a hard coupling between two independent
///          binaries and makes it impossible to evolve them independently.
///          The bootloader's PSW setup is intentionally minimal: it only needs
///          disabled-wait PSWs for the pre-DAT window.

#include <arch/s390x/cpu/psw.h>

/// @brief Install disabled-wait PSWs into all six new PSW slots.
void zxfl_psw_install_safe_psws(void) {
    arch_psw_set_raw(PSW_LC_RESTART,  PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1A0ULL);
    arch_psw_set_raw(PSW_LC_EXTERNAL, PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1B0ULL);
    arch_psw_set_raw(PSW_LC_SVC,      PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1C0ULL);
    arch_psw_set_raw(PSW_LC_PROGRAM,  PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1D0ULL);
    arch_psw_set_raw(PSW_LC_MCCK,     PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1E0ULL);
    arch_psw_set_raw(PSW_LC_IO,       PSW_MASK_DISABLED_WAIT, 0x000000000DEAD1F0ULL);
}
