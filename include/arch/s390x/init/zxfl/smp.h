// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/smp.h
//
/// @brief SMP CPU enumeration and AP control for the ZXFL bootloader.
///
///        CPU discovery uses STSI (Store System Information) instruction,
///        specifically topology level 15.1.2 (CPU topology).  This avoids
///        SCLP entirely, which has known issues on Hercules.
///
///        STSI 15.1.2 returns a topology list describing all CPUs in the
///        configuration.  We walk the list and record each CPU's address
///        and type.  If STSI 15.1.2 is not supported (CC != 0), we fall
///        back to STSI 1.1.1 which gives only the total CPU count; in that
///        case we synthesize a single-entry map for the BSP.
///
///        AP SIGNALING
///        ============
///        At loader entry, all APs are in the stopped state.  The loader
///        does not start APs — that is the kernel's job.  However, the
///        loader must ensure APs remain stopped and do not interfere with
///        the boot sequence.  zxfl_smp_stop_aps() issues SIGP STOP to
///        every non-BSP CPU address found in the enumeration.

#ifndef ZXFOUNDATION_ZXFL_SMP_H
#define ZXFOUNDATION_ZXFL_SMP_H

#include <zxfoundation/types.h>
#include <arch/s390x/init/zxfl/zxfl.h>

/// @brief SIGP order codes (z/Architecture PoP, Chapter 14).
#define SIGP_STOP               0x01U   ///< Stop the target CPU
#define SIGP_SENSE              0x09U   ///< Sense CPU status
#define SIGP_SET_ARCH           0x12U   ///< Set architecture mode

/// @brief SIGP condition codes.
#define SIGP_CC_OK              0U      ///< Order accepted
#define SIGP_CC_STATUS_STORED   1U      ///< Status stored (sense)
#define SIGP_CC_BUSY            2U      ///< CPU busy, retry
#define SIGP_CC_NOT_OPERATIONAL 3U      ///< CPU not operational

/// @brief SIGP status bits (returned in R1 after SENSE).
#define SIGP_STATUS_STOPPED     (1U << 6)   ///< CPU is in stopped state

/// @brief Maximum SIGP retries before giving up on a busy CPU.
#define SIGP_MAX_RETRIES        64U

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/// @brief Enumerate all CPUs using STSI.
///
///        Fills @p buf with up to @p max entries.  The BSP (current CPU)
///        is always entry 0 with state ZXFL_CPU_ONLINE.
///
/// @param buf  Output buffer for CPU info entries.
/// @param max  Maximum entries to write (should be ZXFL_CPU_MAP_MAX).
/// @return Number of CPUs found (>= 1, always includes BSP).
uint32_t zxfl_smp_enumerate(zxfl_cpu_info_t *buf, uint32_t max);

/// @brief Issue SIGP STOP to all non-BSP CPUs found by zxfl_smp_enumerate().
///
///        Must be called after zxfl_smp_enumerate().  Ensures APs cannot
///        interfere with the kernel's early boot sequence.
///
/// @param cpu_map  CPU map filled by zxfl_smp_enumerate().
/// @param count    Number of valid entries in cpu_map.
/// @param bsp_addr CPU address of the BSP (skip this one).
void zxfl_smp_stop_aps(const zxfl_cpu_info_t *cpu_map,
                       uint32_t count,
                       uint16_t bsp_addr);

/// @brief Read the current CPU address from the CPU address register.
///        On z/Architecture, the CPU address is in bits 16-31 of CR0
///        after STAP (Store CPU Address).
/// @return Current CPU address.
uint16_t zxfl_smp_current_cpu_addr(void);

#endif /* ZXFOUNDATION_ZXFL_SMP_H */
