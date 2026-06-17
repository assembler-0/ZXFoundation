/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/sched/domain_enter.S
/// @brief User-mode entry trampoline — sets stack and jumps to ret_to_user.

    .section .text, "ax", @progbits
    .globl  __domain_enter_user_asm
    .type   __domain_enter_user_asm, @function
    .align  8

/// @brief Enter user mode by loading the prepared return frame.
///   %r2 = frame_base (stack pointer with GPRs + PSW laid out)
__domain_enter_user_asm:
    lgr     %r15, %r2
    jg      ret_to_user

    .size   __domain_enter_user_asm, . - __domain_enter_user_asm
    .section .note.GNU-stack, "", @progbits
