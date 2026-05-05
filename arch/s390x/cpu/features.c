// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/features.c
//
/// @brief Boot-time CPU feature detection via STFLE facility list.
#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/stfle.h>
#include <arch/s390x/cpu/features.h>

static bool sys_features_table[] = {
    [ZX_SYS_FEATURE_DIAG44] = false,
    [ZX_SYS_FEATURE_EDAT1]  = false,
    [ZX_SYS_FEATURE_EDAT2]  = false,
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

#define detect_and_count(slot, expr) \
    do { sys_features_table[(slot)] = (expr); if (sys_features_table[(slot)]) ++count; } while (0)

/// @brief Called once from zxfoundation_global_initialize() with the
///        boot protocol so we can inspect the STFLE facility list.
/// @param boot  Validated ZXFL boot protocol pointer
/// @return numer of features detected
int arch_cpu_features_init(const zxfl_boot_protocol_t *boot) {
    if (!boot) return 0;

    if (boot->stfle_count < 1) return 0;

    int count = 0;

    if (boot->stfle_count >= 2)
        detect_and_count(ZX_SYS_FEATURE_DIAG44,
                         stfle_has_facility(boot->stfle_fac, 74U));

    /* EDAT-1: bit 8 lives in dword 0. */
    detect_and_count(ZX_SYS_FEATURE_EDAT1,
                     stfle_has_facility(boot->stfle_fac, STFLE_BIT_EDAT1));

    /* EDAT-2: bit 78 lives in dword 1. */
    if (boot->stfle_count >= 2)
        detect_and_count(ZX_SYS_FEATURE_EDAT2,
                         stfle_has_facility(boot->stfle_fac, STFLE_BIT_EDAT2));

    return count;
}
