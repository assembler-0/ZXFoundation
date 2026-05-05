// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/processor.h
//
/// @brief s390x CPU control primitives

#pragma once

#include <arch/s390x/cpu/atomic.h>
#include <zxfoundation/zconfig.h>
#include <arch/s390x/cpu/features.h>

/// @brief Generic spin-loop hint.  Use inside every busy-wait loop.
static inline void arch_cpu_relax(void) {
    barrier();
}

/// @brief Yield the virtual CPU timeslice to the hypervisor.
///        Only issues DIAG 44 when confirmed safe at runtime.
///        Do NOT use as a drop-in for cpu_relax() in tight spin loops.
static inline void arch_smp_yield(void) {
    if (arch_cpu_has_sys_feature(ZX_SYS_FEATURE_DIAG44))
        __asm__ volatile ("diag 0,0,0x44" ::: "memory");
    else
        arch_cpu_relax();
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

#define arch_cpu_addr(addr) __asm__ volatile ("stap %0" : "=Q" (addr))

/// @brief Get the logical processor ID for the executing CPU.
static inline int arch_smp_processor_id(void) {
    uint16_t cpu_addr;
    arch_cpu_addr(cpu_addr);
    return cpu_addr & 0x3F;
}

/// @brief Enter the disabled wait state.
[[noreturn]] static inline void arch_sys_halt(void) {
    __asm__ volatile(
        "   larl    %%r1, 1f\n"
        "   lpswe   0(%%r1)\n"
        "   .align  8\n"
        "1: .quad   %0, %1\n"
        :
        : "i"(CONFIG_PSW_DISABLED_WAIT),
          "i"(CONFIG_PANIC_HALT_ADDR)
        : "r1", "memory"
    );
    __builtin_unreachable();
}