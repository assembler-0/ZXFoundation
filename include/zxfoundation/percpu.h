// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/percpu.h
//
/// @brief Per-CPU data access via prefix-relative addressing.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/zconfig.h>

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
/// @param node     NUMA node affinity.
/// @return Physical address of the new lowcore, or 0 on failure.
uint64_t percpu_init_ap(uint16_t cpu_id, uint16_t cpu_addr, uint8_t node);

/// @brief Obtain a pointer to the current CPU's monolithic lowcore.
#define percpu_ptr()            zx_lowcore()

/// @brief Increment a field in the current CPU's per-CPU area (in place).
#define percpu_inc(field)       do { percpu_ptr()->percpu.field++; } while (0)

/// @brief Decrement a field in the current CPU's per-CPU area (in place).
#define percpu_dec(field)       do { percpu_ptr()->percpu.field--; } while (0)

/// @brief Read a field from the current CPU's per-CPU software block.
#define percpu_get(field)       read_once((percpu_ptr()->percpu.field))

/// @brief Write a field in the current CPU's per-CPU area.
#define percpu_set(field, val)  do { write_once(percpu_ptr()->percpu.field, (val)); } while (0)

/// @brief Obtain a pointer to a field in the current CPU's per-CPU area.
#define percpu_ptr_to(field)    (&(percpu_ptr()->percpu.field))

/// @brief Read a field from another CPU's per-CPU area.
#define percpu_get_on(cpu, field) read_once((percpu_areas[cpu]->percpu.field))

/// @brief Write a field in another CPU's per-CPU area.
#define percpu_set_on(cpu, field, val) do { write_once(percpu_areas[cpu]->percpu.field, (val)); } while (0)

/// @brief Obtain a pointer to a field in another CPU's per-CPU area.
#define percpu_ptr_on(cpu, field) (&(percpu_areas[cpu]->percpu.field))

