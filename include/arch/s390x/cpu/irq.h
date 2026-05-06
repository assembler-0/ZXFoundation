#pragma once

#include <zxfoundation/types.h>

typedef uint64_t irqflags_t;

static inline irqflags_t arch_local_save_flags(void) {
    uint32_t hi, lo;
    __asm__ volatile (
        "epsw %[hi], %[lo]\n"
        : [hi] "=d" (hi), [lo] "=d" (lo)
        :
        : "cc"
    );
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void arch_local_irq_disable(void) {
    uint8_t dummy;
    __asm__ volatile (
        "stnsm  %0, 0xFC\n"
        : "=Q" (dummy)
        :
        : "memory"
    );
}

static inline void arch_local_irq_restore(irqflags_t flags) {
    const uint8_t mask = (uint8_t)(flags >> 56);
    const uint8_t irq_mask = mask & 0x03U;
    uint8_t dummy;
    if (irq_mask == 0x03U) {
        __asm__ volatile ("stosm %0, 0x03\n" : "=Q" (dummy) :: "memory");
    } else if (irq_mask == 0x02U) {
        __asm__ volatile ("stosm %0, 0x02\n" : "=Q" (dummy) :: "memory");
    } else if (irq_mask == 0x01U) {
        __asm__ volatile ("stosm %0, 0x01\n" : "=Q" (dummy) :: "memory");
    }
}
