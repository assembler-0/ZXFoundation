// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/features.c
//
/// @brief Boot-time CPU feature detection.
///
///        zx_has_diag44 is set if the kernel is running under a hypervisor
///        that supports DIAG 44 (z/VM, Hercules).  We detect this via
///        STFLE facility bit 74 ("DIAG 44 is available"), which is set by
///        z/VM in the guest's facility list.  On bare metal bit 74 is 0.

#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/init/zxfl/zxfl.h>

bool zx_has_diag44 = false;

/// @brief Called once from zxfoundation_global_initialize() with the
///        boot protocol so we can inspect the STFLE facility list.
/// @param boot  Validated ZXFL boot protocol pointer.
void zx_cpu_features_init(const zxfl_boot_protocol_t *boot) {
    if (!boot)
        return;

    if (boot->stfle_count > 1) {
        const uint64_t diag44_mask = UINT64_C(0x0020000000000000);
        zx_has_diag44 = (boot->stfle_fac[1] & diag44_mask) != 0;
    }
}
