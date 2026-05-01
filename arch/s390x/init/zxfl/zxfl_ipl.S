// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/zxfl_ipl.S - ZXFoundation Loader Entry
//
/// @brief IPL entry point and stack setup.
///
/// EXECUTION ENVIRONMENT
/// =====================
/// All IBM z Systems start in ESA/390-compatibility mode after IPL (PoO
/// §4.2.1), regardless of the ARCHMODE setting in Hercules.  The CPU is
/// in 31-bit addressing mode with ESA/390 PSW format and ESA/390 lowcore
/// layout.  The SIGP Set Architecture in zxfl_jump_to_kernel switches to
/// z/Arch after the bootloader has finished its work.
///
/// Every instruction in this file must be valid ESA/390.

#include <zxfoundation/zconfig.h>

    .section .text.ipl, "ax"
    .globl __zx_ipl_entry
    .org 0x0

__zx_ipl_entry:
    // IPL PSW (8 bytes at 0x000).
    // Word 0: 0x00080000 — 31-bit addressing, supervisor state.
    // Word 1: 0x80000000 | zxfl_loader_entry — 31-bit branch target.
    .long   0x00080000
    .long   0x80000000 + zxfl_loader_entry

    // IPL CCW1 (8 bytes at 0x008) — reserved for hardware
    .long   0x00000000, 0x00000000
    // IPL CCW2 (8 bytes at 0x010) — reserved
    .long   0x00000000, 0x00000000

    // -----------------------------------------------------------------------
    .section .text, "ax"
    .globl zxfl_loader_entry
zxfl_loader_entry:
    // Set up the C stack.  Reserve the standard 160-byte save area below
    // the stack pointer as required by the ESA/390 ABI.
    larl    %r15, zxfl_stack_top
    ahi     %r15, -CONFIG_S390X_SAVE_AREA

    brasl   %r14, zxfl_main

    // zxfl_main is [[noreturn]] via zxfl_jump_to_kernel.
    // If it returns (error path), enter a disabled-wait PSW.
.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpsw    0(%r1)
    j       .Lhalt

    // ESA/390 disabled-wait PSW (8 bytes, 4-byte aligned):
    //   Word 0: 0x000A0000 — wait state, 31-bit addressing
    //   Word 1: 0xDEADDEAD — visually distinct in Hercules logs
    .align 4
.Ldisabled_wait_psw:
    .long   0x000A0000
    .long   0xDEADDEAD

    // -----------------------------------------------------------------------
    .section .bss
    .align 16
zxfl_stack_bottom:
    .skip   CONFIG_BOOT_STACK_SIZE
zxfl_stack_top:

    .section .note.GNU-stack, "", @progbits
