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
#include <arch/s390x/cpu/features.h>
#include <zxfoundation/sys/printk.h>

static bool sys_features_table[] = {
    [ZX_SYS_FEATURE_DIAG44] = false,
    [ZX_SYS_FEATURE_EDAT1] = false,
};
static constexpr int sys_features_table_size = sizeof(sys_features_table);

static bool check_valid_sys_feature(const uint32_t feature) {
    return feature < sys_features_table_size;
}

/// @brief determine if a system feature is present.
bool arch_cpu_has_sys_feature(const uint32_t feature) {
    if (!check_valid_sys_feature(feature))
        return false;
    return sys_features_table[feature];
}

#define check_and_inc(c) if ((c)) ++count;

/// @brief Called once from zxfoundation_global_initialize() with the
///        boot protocol so we can inspect the STFLE facility list.
/// @param boot  Validated ZXFL boot protocol pointer
/// @return numer of features detected
int arch_cpu_features_init(const zxfl_boot_protocol_t *boot) {
    if (!boot)
        return 0;

    int count = 0;

    if (boot->stfle_count > 1)
        return 0;

    constexpr uint64_t diag44_mask = UINT64_C(0x0020000000000000);
    check_and_inc(sys_features_table[ZX_SYS_FEATURE_DIAG44] = (boot->stfle_fac[1] & diag44_mask) != 0);
    check_and_inc(sys_features_table[ZX_SYS_FEATURE_EDAT1] = stfle_has_facility(boot->stfle_fac, STFLE_BIT_EDAT1))

    printk("sys: arch_cpu_features_init: %d features detected\n", count);

    return count;
}

