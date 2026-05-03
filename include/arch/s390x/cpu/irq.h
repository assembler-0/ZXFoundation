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

/// @brief Disable all maskable interrupts on the local CPU (I/O and External).
///        Uses STNSM to clear bits 6 and 7 of the PSW, returning the old mask byte.
static inline void arch_local_irq_disable(void) {
    uint8_t old_mask;
    // 0xFC = 11111100 in binary. Clears bits 6 and 7, preserves bits 0-5 (including DAT).
    __asm__ volatile (
        "stnsm  %0, 0xFC\n"
        : "=Q" (old_mask)
        :
        : "memory"
    );
}

/// @brief Restore the PSW mask to a previously saved value.
///        Uses STOSM to set bits that were previously set.
static inline void arch_local_irq_restore(irqflags_t flags) {
    uint8_t mask_byte = (uint8_t)(flags >> 56);
    // STOSM ORs the operand into the system mask.
    // Wait, STOSM only sets bits, it doesn't clear them if they were 0.
    // It's safer to use SSM if we want exact restoration, but we MUST preserve DAT!
    // Since we only manipulate bits 6 and 7, and 'flags' has the exact byte 0,
    // we can just SSM the entire byte.
    __asm__ volatile (
        "ssm    %0\n"
        :
        : "Q" (mask_byte)
        : "memory"
    );
}