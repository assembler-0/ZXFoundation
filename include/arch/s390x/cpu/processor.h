// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/processor.h
//
/// @brief s390x CPU control primitives: spin hints and hypervisor yield.
///
///        cpu_relax()
///        ===========
///        On s390x there is no ISA-level spin-wait hint (no equivalent of
///        x86 PAUSE or ARM YIELD).  A compiler barrier is the correct
///        implementation: it prevents the compiler from collapsing or
///        hoisting the spin-loop body while adding zero hardware overhead.
///        The CPU's out-of-order engine handles bus traffic naturally.
///
///        zx_smp_yield()
///        ==============
///        DIAG 44 is a hypervisor diagnose call (z/VM, Hercules).  On bare
///        metal it causes a privileged-operation exception from problem state
///        and is implementation-defined in supervisor state — it must NOT be
///        called unconditionally.  zx_smp_yield() gates the call on a
///        boot-time flag (zx_has_diag44) populated from STFLE/SCLP during
///        early init.  Use it only in explicit yield points (idle loop,
///        long-poll paths) — never as a generic spin hint.

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
