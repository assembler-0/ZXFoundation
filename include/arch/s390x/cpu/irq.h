#pragma once

#include <zxfoundation/types.h>

/// @brief s390x PSW mask type — holds the full 64-bit PSW mask word.
typedef uint64_t irqflags_t;

/// @brief Read the current PSW mask (EPSW instruction).
static inline irqflags_t arch_local_save_flags(void) {
    uint32_t hi, lo;
    // EPSW Rx, Ry: stores PSW bits 0-31 into Rx, bits 32-63 into Ry.
    __asm__ volatile (
        "epsw %[hi], %[lo]\n"
        : [hi] "=d" (hi), [lo] "=d" (lo)
        :
        : "cc"
    );
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/// @brief Disable all maskable interrupts on the local CPU.
///        SSM 0 clears the I/O, external, and machine-check mask bits.
static inline void arch_local_irq_disable(void) {
    uint8_t zero = 0x00;
    __asm__ volatile (
        "ssm    %0\n"
        :
        : "Q" (zero)
        : "memory"
    );
}

/// @brief Restore the PSW mask to a previously saved value.
static inline void arch_local_irq_restore(irqflags_t flags) {
    uint8_t mask_byte = (uint8_t)(flags >> 56);
    __asm__ volatile (
        "ssm    %0\n"
        :
        : "Q" (mask_byte)
        : "memory"
    );
}