// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/psw.h
//
/// @brief Unified z/Architecture PSW manager.
///
///        This is the single authoritative source for all PSW mask constants
///        and new-PSW lowcore offsets.  It is safe to include from both C
///        translation units and assembly files (.S).
///
///        z/Architecture PSW mask word bit layout (PoP SA22-7832, Figure 4-1):
///
///          Bit  0     (0x8000000000000000) PER mask
///          Bit  1     (0x4000000000000000) reserved (must be 0)
///          Bit  2     (0x2000000000000000) reserved (must be 0)
///          Bit  3     (0x1000000000000000) reserved (must be 0)
///          Bit  4     (0x0800000000000000) reserved (must be 0)
///          Bit  5     (0x0400000000000000) DAT (address translation enable)
///          Bit  6     (0x0200000000000000) I/O interrupt mask
///          Bit  7     (0x0100000000000000) External interrupt mask
///          Bits 8-11  (0x00F0000000000000) Storage key
///          Bit 12     (0x0008000000000000) Machine-check mask
///          Bit 13     (0x0004000000000000) reserved (must be 0)
///          Bit 14     (0x0002000000000000) Wait state
///          Bit 15     (0x0001000000000000) Problem state (user mode)
///          Bits 16-17 (0x0000C00000000000) Address space control
///          Bits 18-19 (0x0000300000000000) Condition code
///          Bits 20-23 (0x00000F0000000000) Program mask
///          Bit 31     (0x0000000100000000) EA (extended addressing — 64-bit)
///          Bit 32     (0x0000000080000000) BA (basic addressing — 64-bit)
///
///        Setting any reserved bit causes a Specification Exception (ILC=0)
///        when the PSW is loaded via LPSWE.

#pragma once

/// DAT (address translation) enable bit.
#define PSW_BIT_DAT             0x0400000000000000ULL
/// I/O interrupt mask bit.
#define PSW_BIT_IO              0x0200000000000000ULL
/// External interrupt mask bit.
#define PSW_BIT_EXT             0x0100000000000000ULL
/// Machine-check mask bit.
#define PSW_BIT_MCCK            0x0008000000000000ULL
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
/// Used for early boot and AP restart PSW before DAT is enabled.
#define PSW_MASK_KERNEL         (PSW_ARCH_BITS)

/// Supervisor mode, DAT on, all interrupts disabled, 64-bit.
/// Used for the DAT-on transition PSW (return_psw in ap_entry).
#define PSW_MASK_KERNEL_DAT     (PSW_BIT_DAT | PSW_ARCH_BITS)

/// Disabled wait: wait state set, DAT off, all interrupts disabled, 64-bit.
/// The wait bit (bit 14) is the only correct way to enter a wait state.
/// The old hardcoded value 0x0000800180000000 was WRONG — bit 16 is an
/// address-space control bit, not the wait bit.
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
static inline void psw_set_raw(uint64_t lc_offset, uint64_t mask, uint64_t addr) {
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

/// @brief Install disabled-wait PSWs into all six new PSW slots.
void psw_install_new_psws(void);

#endif /* __ASSEMBLER__ */
