// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/zxconfig.h

#pragma once

/// @brief kernel release
#define CONFIG_ZX_RELEASE           "26h1"

/// @brief kernel copyright date
#define CONFIG_ZX_COPYRIGHT_DATE    "2026"

/// @brief kernel build time (from compiler)
#define CONFIG_ZX_BUILD_DT_COMPILER  __DATE__ " " __TIME__

/// @brief host build platform
#define CONFIG_ZX_BUILD_PLATFORM    "Darwin@25.5.0::x86_64"

/// @brief compiler id
#define CONFIG_ZX_COMPILER_ID       __VERSION__

/// @brief maximum number of CPUs
#define CONFIG_ZX_MAX_CPUS           128U
