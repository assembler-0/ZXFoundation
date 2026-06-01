// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/features.c
//
/// @brief Boot-time CPU feature detection via STFLE facility list.
#include <arch/s390x/cpu/stfle.h>
#include <arch/s390x/cpu/features.h>
#include <zxfoundation/common.h>

bool sys_features_table[] = {
    [ZX_SYS_FEATURE_DIAG44] = false,
    [ZX_SYS_FEATURE_EDAT1]  = false,
    [ZX_SYS_FEATURE_EDAT2]  = false,
    [ZX_SYS_FEATURE_PFMF]   = false,
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

/// @brief Called once from zxfoundation_global_initialize() with the
///        boot protocol so we can inspect the STFLE facility list.
/// @return numer of features detected
int arch_cpu_features_init(uint64_t *fac_list, const uint32_t stfle_count) {
#define detect_and_count(bit) \
    do { sys_features_table[concat(ZX_SYS_FEATURE_, bit)] = stfle_has_facility(fac_list, concat(STFLE_BIT_, bit)); sys_features_table[concat(ZX_SYS_FEATURE_, bit)] ? ++count : 0; } while (0)
#define detect_and_count_raw(slot, bit) \
    do { sys_features_table[(slot)] = stfle_has_facility(fac_list, (bit)); sys_features_table[(slot)] ? ++count : 0; } while (0)

    if (stfle_count < 1) return 0;

    int count = 0;

    if (stfle_count >= 2)
        detect_and_count_raw(ZX_SYS_FEATURE_DIAG44, 74U);

    /* EDAT-1: bit 8 lives in dword 0. */
    detect_and_count(EDAT1);

    /* PFMF: bit 14 lives in dword 0. */
    detect_and_count(PFMF);

    /* EDAT-2: bit 78 lives in dword 1. */
    if (stfle_count >= 2)
        detect_and_count(EDAT2);

#undef detect_and_count
#undef detect_and_count_raw
    return count;
}
