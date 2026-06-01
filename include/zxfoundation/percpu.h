// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/percpu.h
//
/// @brief Per-CPU data access via prefix-relative addressing.

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/zxconfig.h>
#include <zxfoundation/memory/hhdm.h>
#include <arch/s390x/cpu/lowcore.h>

#define MAX_CPUS        CONFIG_ZX_MAX_CPUS

/// @brief Global raw array of lowcore pointers. DO NOT ACCESS DIRECTLY.
///        Use zx_lowcore_cpu(cpu) to safely bypass hardware prefix aliasing.
extern zx_lowcore_t *__percpu_areas_raw[MAX_CPUS];

/// @brief Safely retrieve another CPU's lowcore, applying inverse hardware
///        prefix-register swapping if necessary.
#define zx_lowcore_cpu(cpu)                                                          \
    ({                                                                               \
        zx_lowcore_t *__lc = __percpu_areas_raw[(cpu)];                              \
        zx_lowcore_t *__res = __lc;                                                  \
        if (__lc) {                                                                  \
            uint64_t __target_real = hhdm_virt_to_phys_inplace((uint64_t)__lc);      \
            uint64_t __my_prefix = zx_lowcore()->percpu.prefix_base;                 \
            if (__target_real == __my_prefix) {                                      \
                __res = (zx_lowcore_t *)CONFIG_KERNEL_VIRT_OFFSET;                   \
            } else if (__target_real == 0) {                                         \
                __res = (zx_lowcore_t *)hhdm_phys_to_virt_inplace(__my_prefix);      \
            }                                                                        \
        }                                                                            \
        __res;                                                                       \
    })


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
#define percpu_get_on(cpu, field) read_once((zx_lowcore_cpu(cpu)->percpu.field))

/// @brief Write a field in another CPU's per-CPU area.
#define percpu_set_on(cpu, field, val) do { write_once(zx_lowcore_cpu(cpu)->percpu.field, (val)); } while (0)

/// @brief Obtain a pointer to a field in another CPU's per-CPU area.
#define percpu_ptr_on(cpu, field) (&(zx_lowcore_cpu(cpu)->percpu.field))

