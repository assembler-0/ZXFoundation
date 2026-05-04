#pragma once

#define ZX_SYS_FEATURE_DIAG44 0
#define ZX_SYS_FEATURE_EDAT1  1

#include <arch/s390x/init/zxfl/zxfl.h>

/// @brief determine if a system feature is present.
bool arch_cpu_has_sys_feature(const uint32_t feature);

/// @brief Called once from zxfoundation_global_initialize() with the
///        boot protocol so we can inspect the STFLE facility list.
/// @param boot  Validated ZXFL boot protocol pointer
/// @return numer of features detected
int arch_cpu_features_init(const zxfl_boot_protocol_t *boot);