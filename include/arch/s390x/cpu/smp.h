// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/smp.h
#pragma once

#include <arch/s390x/init/zxfl/zxfl.h>

/// @brief Phase 1 of AP bringup: allocate a lowcore and stacks for every
///        stopped AP in the boot protocol, populate percpu_areas[], and
///        install all PSWs and control registers into each AP lowcore.
///        Does NOT issue SIGP RESTART — APs remain stopped.
/// @param boot  Validated ZXFL boot protocol.
void smp_prepare_aps(const zxfl_boot_protocol_t *boot);

/// @brief Phase 2 of AP bringup: issue SIGP SET_PREFIX + SIGP RESTART to
///        every AP prepared by smp_prepare_aps(), then spin until all APs
///        have incremented ap_online_count from ap_startup().
///
///        Must be called after smp_prepare_aps() and after all kernel
///        subsystems (IRQ, slab, VMM) are fully initialized.
void smp_start_aps(void);

/// @brief Shut down all APs that were started by smp_start_aps().
void smp_teardown(void);

/// @brief Issue SIGP STOP to every CPU in the boot map except the caller.
///        Lock-free; safe to call from any context including the halt path.
/// @param cpu_map    Boot protocol CPU map array.
/// @param cpu_count  Number of entries.
void smp_stop_all_raw(const zxfl_cpu_info_t *cpu_map, uint32_t cpu_count);