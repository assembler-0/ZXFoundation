// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/percpu.h
//
/// @brief Per-CPU data access via prefix-relative addressing.
///
///        LAYOUT
///        ======
///        Each CPU's prefix area (lowcore) is a monolithic 8 KB block.
///        The hardware lowcore occupies [0x000, 0x400) and [0x1200, 0x2000);
///        the software per-CPU block (zx_percpu_t) occupies [0x400, 0x1200).
///
///        The prefix register (SPX) points to the base of the lowcore.
///        All per-CPU accesses are computed as:
///
///            *(type *)(prefix_base + 0x400 + offsetof(zx_percpu_t, field))
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

/// Forward declaration of the monolithic lowcore structure.
/// The full definition is in <arch/s390x/cpu/lowcore.h>.
typedef struct zx_lowcore zx_lowcore_t;

#define MAX_CPUS        CONFIG_ZX_MAX_CPUS

/// @brief Global array of lowcore pointers, indexed by logical CPU ID.
extern zx_lowcore_t *percpu_areas[MAX_CPUS];

/// @brief Initialize the BSP's per-CPU area.  Called once from main.c.
void percpu_init_bsp(void);

/// @brief Allocate and initialize a per-CPU area for an AP.
///        Returns the physical address of the new lowcore (for SPX).
/// @param cpu_id   Logical CPU ID (1..MAX_CPUS-1).
/// @param cpu_addr z/Arch CPU address (from boot protocol).
/// @return Physical address of the new lowcore, or 0 on failure.
uint64_t percpu_init_ap(uint16_t cpu_id, uint16_t cpu_addr);

/// @brief Obtain a pointer to the current CPU's monolithic lowcore.
#define percpu_ptr() (percpu_areas[arch_smp_processor_id()])

/// @brief Read a field from the current CPU's per-CPU software block.
#define percpu_get(field)       (percpu_ptr()->percpu.field)

/// @brief Increment a field in the current CPU's per-CPU area (in place).
#define percpu_inc(field)       do { percpu_ptr()->percpu.field++; } while (0)

/// @brief Decrement a field in the current CPU's per-CPU area (in place).
#define percpu_dec(field)       do { percpu_ptr()->percpu.field--; } while (0)

/// @brief Write a field in the current CPU's per-CPU area.
#define percpu_set(field, val)  do { percpu_ptr()->percpu.field = (val); } while (0)

/// @brief Read a field from another CPU's per-CPU area.
#define percpu_get_on(cpu, field) (percpu_areas[cpu]->percpu.field)

/// @brief Write a field in another CPU's per-CPU area.
#define percpu_set_on(cpu, field, val) do {                         \
    percpu_areas[cpu]->percpu.field = (val);                       \
} while (0)

