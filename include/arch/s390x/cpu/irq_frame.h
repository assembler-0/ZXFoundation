// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/irq_frame.h
//
/// @brief z/Architecture interrupt frame (pt_regs equivalent).
///
///        The frame is built on the async or machine-check stack by the
///        low-level entry stubs in arch/s390x/trap/entry.S.  It captures
///        the full general-purpose register file and the old PSW that the
///        hardware saved into the lowcore before dispatching the interrupt.
///
///        Layout (all fields 8 bytes, total 160 bytes):
///
///          Offset  Field
///          ------  -----
///          0x00    gprs[0]  – gprs[15]   (128 bytes)
///          0x80    psw_mask              (8 bytes)
///          0x88    psw_addr              (8 bytes)
///
///        The frame is always 8-byte aligned.  The entry stubs allocate it
///        by decrementing %r15 by IRQ_FRAME_SIZE + 160 (standard save area)
///        before storing registers.

#pragma once

/// Frame field offsets — used by entry.S to store/load without C structs.
#define IRQ_FRAME_GPRS      0x00
#define IRQ_FRAME_PSW_MASK  0x80
#define IRQ_FRAME_PSW_ADDR  0x88
/// Total frame size: 16 GPRs (128) + PSW mask (8) + PSW addr (8) = 144 = 0x90.
#define IRQ_FRAME_SIZE      0x90

#ifndef __ASSEMBLER__

#include <zxfoundation/types.h>

/// @brief Interrupt frame saved by entry.S on every trap/interrupt.
typedef struct __attribute__((packed, aligned(8))) {
    uint64_t gprs[16];  ///< General-purpose registers r0–r15 at interrupt time.
    uint64_t psw_mask;  ///< Old PSW mask word (hardware-saved in lowcore).
    uint64_t psw_addr;  ///< Old PSW instruction address (hardware-saved in lowcore).
} zx_irq_frame_t;

_Static_assert(sizeof(zx_irq_frame_t) == IRQ_FRAME_SIZE, "zx_irq_frame_t must be 144 bytes");

/// @brief Interrupt class tag — identifies which lowcore new-PSW slot fired.
typedef enum {
    ZX_IRQ_CLASS_PGM  = 0,  ///< Program check (0x01D0).
    ZX_IRQ_CLASS_EXT  = 1,  ///< External interrupt (0x01B0).
    ZX_IRQ_CLASS_IO   = 2,  ///< I/O interrupt (0x01F0).
    ZX_IRQ_CLASS_MCCK = 3,  ///< Machine check (0x01E0).
    ZX_IRQ_CLASS_SVC  = 4,  ///< Supervisor call (0x01C0).
} zx_irq_class_t;

#endif /* __ASSEMBLER__ */
