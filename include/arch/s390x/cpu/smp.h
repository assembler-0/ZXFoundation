// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/smp.h
#pragma once

#include <arch/s390x/init/zxfl/zxfl.h>

/// @brief Bring up all stopped APs listed in the boot protocol.
///        Writes restart PSW, allocates lowcore + stack, issues SIGP.
/// @param boot  Validated ZXFL boot protocol.
void smp_init(const zxfl_boot_protocol_t *boot);

/// @brief Shut down all APs that were started by smp_init().
void smp_teardown();