// SPDX-License-Identifier: Apache-2.0
// @file arch/s390x/cpu/sie.S
// @brief SIE (Start Interpretive Execution) assembly entry point.
//
// Reference: IBM z/Architecture PoP, Chapter 14 (SIE).
// SIE is RRE-format, opcode B25C.
//   R1 = reserved (must be 0)
//   R2 = register holding SCB address
// SIE updates the SCB in-place and exits on intercept.

#include <arch/s390x/cpu/irq_frame.h>

    .section .text, "ax"
    .globl  sie_enter_asm
    .type   sie_enter_asm, @function
    .align  8

// u32 sie_enter_asm(state_control_block* scb)
// r2 = scb pointer (argument)
// Returns: 0 (caller reads SCB fields for exit details)
sie_enter_asm:
    // Preserve registers r6-r15 per s390x ABI
    stmg    %r6, %r15, 48(%r15)
    // Also preserve r0-r5 (including r2 which holds SCB)
    stmg    %r0, %r5, 0(%r15)

    // Execute SIE instruction: R1=0 (reserved), R2=scb (in r2)
    // Opcode B25C, encoded as 0xB25C0000 with R1/R2 fields zeroed for GAS
    .insn   rre, 0xb25c0000, 0, %r2

    // SIE returns here on intercept or interrupt.
    // Result fields are inside the SCB; just return 0.
    lghi    %r2, 0

    // Restore registers
    lmg     %r0, %r5, 0(%r15)
    lmg     %r6, %r15, 48(%r15)
    br      %r14

    .size   sie_enter_asm, . - sie_enter_asm

    .section .note.GNU-stack, "", @progbits
