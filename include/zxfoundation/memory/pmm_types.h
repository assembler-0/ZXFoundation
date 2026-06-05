// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/memory/pmm_types.h
//
/// @brief Core PMM types and constants to break include cycles.

#pragma once

#include <zxfoundation/types.h>

/// Per-CPU page cache sizing.
#define PCP_HIGH            32U    ///< Hot magazine maximum depth.
#define PCP_BATCH           16U    ///< Pages moved per refill / drain.

/// @brief Physical memory zone identifiers.
typedef enum {
    ZONE_DMA    = 0,
    ZONE_NORMAL = 1,
    ZONE_MAX    = 2,
} zone_id_t;

/// @brief Per-CPU order-0 page cache, one per zone.
///        Embedded in zx_percpu_t (lowcore.h).  Access is IRQ-disabled; no
///        spinlock needed because the cache is strictly per-CPU.
typedef struct __attribute__((aligned(64))) pmm_pcplist {
    uint32_t    count_hot;                          ///< Pages in the hot magazine.
    uint32_t    count_cold;                         ///< Pages in the cold magazine.
    uint8_t     zone_id;                            ///< Owning zone (zone_id_t).
    uint8_t     numa_node;                          ///< NUMA node this PCP was last refilled from.
    uint8_t     _pad[6];
    uint64_t    hot [PCP_HIGH + PCP_BATCH];         ///< Hot magazine: PFN stack (index 0 = bottom).
    uint64_t    cold[PCP_HIGH];                     ///< Cold magazine: PFN stack.
} pmm_pcplist_t;
