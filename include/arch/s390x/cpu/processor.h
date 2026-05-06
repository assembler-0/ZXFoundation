// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/processor.h
//
/// @brief s390x CPU control primitives

#pragma once

#include <arch/s390x/cpu/atomic.h>
#include <zxfoundation/zconfig.h>
#include <arch/s390x/cpu/features.h>
#include <arch/s390x/cpu/psw.h>

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

/// @brief stap
static unsigned short arch_cpu_addr(void) {
    unsigned short cpu_address;
    __asm__ volatile("stap %0" : "=m" (cpu_address));
    return cpu_address;
}

/// @brief Get the logical processor ID for the executing CPU.
static inline int arch_smp_processor_id(void) {
    uint16_t cpu_addr = arch_cpu_addr();
    return cpu_addr & 0x3F;
}

/// @brief Get the Time-of-Day clock.
static unsigned long long arch_get_tod_clock(void) {
    unsigned long long clk;
    __asm__ volatile("stck %0" : "=Q" (clk) :: "cc");
    return clk;
}

/// @brief Set the clock comparator.
static inline void arch_set_clock_comparator(uint64_t time) {
    __asm__ volatile("sckc %0" :: "Q" (time));
}

static void arch_set_cpu_timer(uint64_t timer) {
    __asm__ volatile("spt %0" :: "Q" (timer));
}

#define arch_ctl_store(array, low, high) ({		        \
	typedef struct { char _[sizeof(array)]; } addrtype; \
	__asm__ volatile(					                \
		"	stctg	%1,%2,%0\n"		                    \
		: "=Q" (*(addrtype *)(&array))		            \
		: "i" (low), "i" (high));		                \
})

#define arch_ctl_load(array, low, high) ({			        \
	typedef struct { char _[sizeof(array)]; } addrtype; \
	__asm__ volatile(					                \
		"	lctlg	%1,%2,%0\n"	                		\
		: : "Q" (*(addrtype *)(&array)),	        	\
		  "i" (low), "i" (high));		            	\
})

static inline void arch_ctl_set_bit(unsigned int cr, unsigned int bit) {
    unsigned long reg;
    arch_ctl_store(reg, cr, cr);
    reg |= 1UL << bit;
    arch_ctl_load(reg, cr, cr);
}

static inline void arch_ctl_clear_bit(unsigned int cr, unsigned int bit) {
    unsigned long reg;
    arch_ctl_store(reg, cr, cr);
    reg &= ~(1UL << bit);
    arch_ctl_load(reg, cr, cr);
}

/// @brief cpuid structure
struct s390x_cpuid {
    unsigned int version:8;
    unsigned int ident:24;
    unsigned int machine:16;
    unsigned int unused:16;
} __attribute__((packed, aligned(8)));

static inline void arch_get_cpu_id(struct s390x_cpuid *id) {
    __asm__ volatile("stidp %0" : "=m" (*id));
}

static inline int arch_is_zvm(void) {
    struct s390x_cpuid id;
    arch_get_cpu_id(&id);
    return id.version == 0xFF;
}

/// @brief Enter the disabled wait state.
[[noreturn]] static inline void arch_sys_halt(void) {
    __asm__ volatile(
        "   larl    %%r1, 1f\n"
        "   lpswe   0(%%r1)\n"
        "   .align  8\n"
        "1: .quad   %0, %1\n"
        :
        : "i"(PSW_MASK_DISABLED_WAIT),
          "i"(CONFIG_PANIC_HALT_ADDR)
        : "r1", "memory"
    );
    __builtin_unreachable();
}

/// SIGP order codes (PoP SA22-7832 Chapter 4).
#define SIGP_SENSE                      0x01U
#define SIGP_EXTERNAL_CALL              0x02U
#define SIGP_EMERGENCY_SIGNAL           0x03U
#define SIGP_START                      0x04U
#define SIGP_STOP                       0x05U
#define SIGP_RESTART                    0x06U
#define SIGP_STOP_AND_STORE_STATUS      0x09U
#define SIGP_INITIAL_CPU_RESET          0x0BU
#define SIGP_SET_PREFIX                 0x0DU
#define SIGP_STORE_STATUS               0x0EU
#define SIGP_SET_ARCHITECTURE           0x12U
#define SIGP_SET_MULTI_THREADING        0x16U
#define SIGP_STORE_ASTATUS_AT_ADDRESS   0x17U

/// SIGP condition codes.
#define SIGP_CC_ORDER_CODE_ACCEPTED     0
#define SIGP_CC_STATUS_STORED           1
#define SIGP_CC_BUSY                    2
#define SIGP_CC_NOT_OPERATIONAL         3


/// @brief Issue a SIGP (Signal Processor) instruction.
///        The order code is passed in an address register per PoP SA22-7832.
///        On CC=1 (status stored) the CPU status word is written to *status
///        if status is non-null.
/// @param cpu_addr  Target CPU address.
/// @param order     SIGP order code (SIGP_* constants below).
/// @param parm      Parameter (placed in r1; used by Set Prefix, Set Architecture, …).
/// @param status    Out: CPU status word on CC=1, unchanged otherwise.
/// @return Condition code: 0 = accepted, 1 = status stored, 2 = busy, 3 = not operational.
static inline int sigp(uint16_t cpu_addr, uint8_t order, uint32_t parm,
                       uint32_t *status) {
    register uint32_t r1 __asm__("1") = parm;
    int cc;
    __asm__ volatile(
        "sigp %1,%2,0(%3)\n"
        "ipm  %0\n"
        "srl  %0,28\n"
        : "=d" (cc), "+d" (r1)
        : "d" ((uint64_t)cpu_addr), "a" ((uint64_t)order)
        : "cc"
    );
    if (status && cc == 1)
        *status = r1;
    return cc;
}

/// @brief Issue SIGP, retrying until the target CPU is no longer busy (CC≠2).
///        Use this for all orders that must not be silently dropped.
static inline int sigp_busy(uint16_t cpu_addr, uint8_t order, uint32_t parm,
                            uint32_t *status) {
    int cc;
    do {
        cc = sigp(cpu_addr, order, parm, status);
    } while (cc == SIGP_CC_BUSY);
    return cc;
}