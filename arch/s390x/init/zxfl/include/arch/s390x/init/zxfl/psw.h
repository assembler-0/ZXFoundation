// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/init/zxfl/include/arch/s390x/init/zxfl/psw.h
/// @brief z/Architecture PSW .

#pragma once

/// Formerly CONFIG_HALT_ADDR
#define PSW_STD_HALT_ADDR       0x0000000000DEAD00ULL

/// DAT (address translation) enable bit.
#define PSW_BIT_DAT             0x0400000000000000ULL
/// I/O interrupt mask bit.
#define PSW_BIT_IO              0x0200000000000000ULL
/// External interrupt mask bit.
#define PSW_BIT_EXT             0x0100000000000000ULL
/// Machine-check mask bit (PoP bit 13).
#define PSW_BIT_MCCK            0x0004000000000000ULL
/// Wait state bit.
#define PSW_BIT_WAIT            0x0002000000000000ULL
/// Problem state (user mode) bit.
#define PSW_BIT_PSTATE          0x0001000000000000ULL
/// EA bit — required for 64-bit addressing.
#define PSW_BIT_EA              0x0000000100000000ULL
/// BA bit — required for 64-bit addressing.
#define PSW_BIT_BA              0x0000000080000000ULL

/// EA|BA — 64-bit addressing mode bits.  Present in every valid z/Arch PSW.
#define PSW_ARCH_BITS           (PSW_BIT_EA | PSW_BIT_BA)

/// Supervisor mode, DAT off, all interrupts disabled, 64-bit.
#define PSW_MASK_KERNEL         (PSW_ARCH_BITS)

/// Home Space Mode bit mask (ASC bits 16-17 = 11).
#define PSW_BIT_HOME_SPACE      0x0000C00000000000ULL

/// Supervisor mode, DAT on, all interrupts disabled, 64-bit.
#define PSW_MASK_KERNEL_DAT     (PSW_BIT_DAT | PSW_BIT_HOME_SPACE | PSW_ARCH_BITS)

/// Disabled wait: wait state set, DAT off, all interrupts disabled, 64-bit.
#define PSW_MASK_DISABLED_WAIT  (PSW_BIT_WAIT | PSW_ARCH_BITS)

/// Restart new PSW offset.
#define PSW_LC_RESTART          0x01A0ULL
/// External interrupt new PSW offset.
#define PSW_LC_EXTERNAL         0x01B0ULL
/// SVC (supervisor call) new PSW offset.
#define PSW_LC_SVC              0x01C0ULL
/// Program check new PSW offset.
#define PSW_LC_PROGRAM          0x01D0ULL
/// Machine-check new PSW offset.
#define PSW_LC_MCCK             0x01E0ULL
/// I/O interrupt new PSW offset.
#define PSW_LC_IO               0x01F0ULL

#ifndef __ASSEMBLER__

#include <zxfoundation/types.h>

/// @brief 64-bit z/Architecture PSW (16 bytes, 8-byte aligned).
typedef struct __attribute__((packed, aligned(8))) {
    uint64_t mask;  ///< PSW mask word (bits 0–63 per PoP Figure 4-1).
    uint64_t addr;  ///< Instruction address (bits 64–127).
} zx_psw_t;

/// @brief Write a PSW pair directly to a physical lowcore offset.
/// @param lc_offset  Lowcore offset (PSW_LC_* constant).
/// @param mask       PSW mask word.
/// @param addr       PSW instruction address.
static inline void arch_psw_set_raw(uint64_t lc_offset, uint64_t mask, uint64_t addr) {
    __asm__ volatile (
        "stg %[mask], 0(%[off])\n"
        "stg %[addr], 8(%[off])\n"
        :
        : [mask] "r" (mask),
          [addr] "r" (addr),
          [off]  "r" (lc_offset)
        : "memory"
    );
}

/// @brief Extract the current PSW via EPSW.
/// @return zx_psw_t with mask populated; addr is not available from EPSW and is set to 0.
static inline zx_psw_t arch_extract_psw(void) {
    uint32_t hi, lo;
    __asm__ volatile("epsw %[hi], %[lo]" : [hi] "=d"(hi), [lo] "=d"(lo) :: "cc");
    return (zx_psw_t){ .mask = ((uint64_t)hi << 32) | (uint64_t)lo, .addr = 0 };
}

/// @brief Load a PSW via LPSWE.  The PSW struct must be 8-byte aligned.
///        This is [[noreturn]] — execution continues at psw->addr.
[[noreturn]] static inline void arch_load_psw(const zx_psw_t *psw) {
    __asm__ volatile("lpswe %0" :: "Q"(*psw) : "memory");
    __builtin_unreachable();
}

#endif /* __ASSEMBLER__ */
