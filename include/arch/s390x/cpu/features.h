#pragma once

#define ZX_SYS_FEATURE_DIAG44 0
#define ZX_SYS_FEATURE_EDAT1  1
#define ZX_SYS_FEATURE_EDAT2  2
#define ZX_SYS_FEATURE_PFMF   3

#include <arch/s390x/init/zxfl/zxfl.h>

/// @breif raw system feature table
/// @warning raw access - no OOB check
extern bool sys_features_table[];

/// @brief determine if a system feature is present.
bool arch_cpu_has_sys_feature(uint32_t feature);

/// @brief Called once from zxfoundation_global_initialize() with the
///        boot protocol so we can inspect the STFLE facility list.
/// @return numer of features detected
int arch_cpu_features_init(uint64_t *fac_list, uint32_t stfle_count);