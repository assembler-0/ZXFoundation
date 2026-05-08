// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/smp.h
#pragma once

#include <arch/s390x/init/zxfl/zxfl.h>

/// @brief Bring up all stopped APs listed in the boot protocol.
///        Writes restart PSW, allocates lowcore + stack, issues SIGP.
/// @param boot  Validated ZXFL boot protocol.
void smp_init(const zxfl_boot_protocol_t *boot);

/// @brief Shut down all APs that were started by smp_init().
void smp_teardown(void);

/// @brief Issue SIGP STOP to every CPU in the boot map except the caller.
///        Lock-free; safe to call from any context including the halt path.
/// @param cpu_map    Boot protocol CPU map array.
/// @param cpu_count  Number of entries.
void smp_stop_all_raw(const zxfl_cpu_info_t *cpu_map, uint32_t cpu_count);