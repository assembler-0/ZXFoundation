// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/processor.h
//
/// @brief s390x CPU control primitives

#pragma once

#include <zxfoundation/atomic.h>
#include <arch/s390x/init/zxfl/zxfl.h>

/// @brief Set to true during early init if DIAG 44 is safe to issue.
extern bool zx_has_diag44;

/// @brief Detect CPU features from the ZXFL boot protocol.
///        Must be called once from zxfoundation_global_initialize().
void zx_cpu_features_init(const zxfl_boot_protocol_t *boot);

/// @brief Generic spin-loop hint.  Use inside every busy-wait loop.
static inline void cpu_relax(void) {
    barrier();
}

/// @brief Yield the virtual CPU timeslice to the hypervisor.
///        Only issues DIAG 44 when confirmed safe at runtime.
///        Do NOT use as a drop-in for cpu_relax() in tight spin loops.
static inline void zx_smp_yield(void) {
    if (zx_has_diag44)
        __asm__ volatile ("diag 0,0,0x44" ::: "memory");
    else
        cpu_relax();
}

/// @brief Set the Storage Key for a 4KB physical block.
/// @param paddr  Physical address (must be 4KB aligned).
/// @param key    Storage key: bits 0-3 = ACC, bit 4 = F, bit 5 = R, bit 6 = C.
static inline void arch_set_storage_key(uint64_t paddr, uint8_t key) {
    __asm__ volatile (
        "sske %[key], %[paddr]\n"
        : : [key] "d" ((uint64_t)key), [paddr] "a" (paddr) : "memory"
    );
}

/// @brief Get the logical processor ID for the executing CPU.
static inline int smp_processor_id(void) {
    uint16_t cpu_addr;
    __asm__ volatile ("stap %0" : "=Q" (cpu_addr));
    return cpu_addr & 0x3F;
}
