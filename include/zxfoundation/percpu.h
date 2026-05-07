// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/percpu.h
//
/// @brief Per-CPU data access via prefix-relative addressing.
///
///        LAYOUT
///        ======
///        Each CPU's lowcore (4 KB, prefix-aligned) is extended with a
///        per-CPU data block starting at offset 0x200 (512 bytes into the
///        lowcore).  The hardware lowcore occupies [0x000, 0x200); the
///        per-CPU block occupies [0x200, 0x1000).
///
///        The prefix register (SPX) points to the base of the lowcore.
///        All per-CPU accesses are computed as:
///
///            *(type *)(prefix_base + 0x200 + offsetof(percpu_t, field))
///
///        STAP (Store CPU Address) returns the current CPU's address, which
///        is used to index into the global percpu_areas[] array to find the
///        prefix_base for a given CPU.
///
///        MACROS
///        ======
///        percpu_get(field)           — read my CPU's field
///        percpu_set(field, val)      — write my CPU's field
///        percpu_get_on(cpu, field)   — read another CPU's field
///        percpu_set_on(cpu, field, val) — write another CPU's field
///
///        INITIALIZATION
///        ==============
///        percpu_init_bsp()  — called once on the BSP to set up its lowcore
///        percpu_init_ap(cpu) — called once per AP during SMP bringup

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/memory/pmm.h>

#define PERCPU_OFFSET   0x200UL                  ///< Offset from prefix base to per-CPU block.
#define MAX_CPUS        CONFIG_ZX_MAX_CPUS

/// @brief Per-CPU data structure.  Lives at prefix + 0x200 for each CPU.
typedef struct percpu {
    uint64_t    prefix_base;        ///< Physical address of this CPU's lowcore.
    uint16_t    cpu_id;             ///< Logical CPU ID (0 = BSP).
    uint16_t    cpu_addr;           ///< z/Arch CPU address (STAP result).
    uint32_t    lock_depth;         ///< Current qspinlock nesting depth.
    mcs_node_t  lock_nodes[MAX_LOCK_DEPTH]; ///< MCS nodes for qspinlock.
    uint64_t    rcu_gp_seq;         ///< RCU grace-period sequence number.
    uint64_t    rcu_qs_seq;         ///< RCU quiescent-state sequence number.
    uint8_t     in_rcu_read_side;   ///< 1 if inside rcu_read_lock().
    uint8_t     _pad0[7];
    uint64_t    ap_stack_top;       ///< AP initial stack pointer (physical, set before SIGP Restart).
    pmm_pcplist_t pcp[ZONE_MAX];    ///< Per-CPU PMM page cache, one per zone.
} percpu_t;

/// @brief Global array of per-CPU area pointers, indexed by logical CPU ID.
extern percpu_t *percpu_areas[MAX_CPUS];

/// @brief Initialize the BSP's per-CPU area.  Called once from main.c.
void percpu_init_bsp(void);

/// @brief Allocate and initialize a per-CPU area for an AP.
///        Returns the physical address of the new lowcore (for SPX).
/// @param cpu_id   Logical CPU ID (1..MAX_CPUS-1).
/// @param cpu_addr z/Arch CPU address (from boot protocol).
/// @return Physical address of the new lowcore, or 0 on failure.
uint64_t percpu_init_ap(uint16_t cpu_id, uint16_t cpu_addr);

/// @brief Obtain a pointer to the current CPU's per-CPU area.
///
///        arch_cpu_addr() issues STAP, which stores the CPU *address* (a
///        16-bit hardware identifier, e.g. 0x0002 for the third CPU).  It
///        is NOT the prefix base and must NOT be used as a pointer directly.
///        arch_smp_processor_id() masks it to a logical index (cpu_addr &
///        0x3F) which is used to index percpu_areas[], giving the correct
///        HHDM-mapped pointer to the per-CPU block.
#define percpu_ptr() (percpu_areas[arch_smp_processor_id()])

/// @brief Read a field from the current CPU's per-CPU area.
#define percpu_get(field)       (percpu_ptr()->field)

/// @brief Increment a field in the current CPU's per-CPU area (in place).
#define percpu_inc(field)       do { percpu_ptr()->field++; } while (0)

/// @brief Decrement a field in the current CPU's per-CPU area (in place).
#define percpu_dec(field)       do { percpu_ptr()->field--; } while (0)

/// @brief Write a field in the current CPU's per-CPU area.
#define percpu_set(field, val)  do { percpu_ptr()->field = (val); } while (0)

/// @brief Read a field from another CPU's per-CPU area.
#define percpu_get_on(cpu, field) (percpu_areas[cpu]->field)

/// @brief Write a field in another CPU's per-CPU area.
#define percpu_set_on(cpu, field, val) do {                         \
    percpu_areas[cpu]->field = (val);                               \
} while (0)
