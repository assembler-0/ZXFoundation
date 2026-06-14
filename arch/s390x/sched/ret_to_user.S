/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/sched/ret_to_user.S
/// @brief Return-to-user trampoline for first user-mode entry.

#include <arch/s390x/cpu/lowcore.h>

#define IRQ_FRAME_GPRS      0x00
#define IRQ_FRAME_PSW       0x80

    .section .text, "ax", @progbits
    .globl  ret_to_user
    .type   ret_to_user, @function
    .align  8

ret_to_user:
    mvc     LC_RETURN_PSW(16, 0), IRQ_FRAME_PSW(%r15)

    lmg     %r0, %r15, IRQ_FRAME_GPRS(%r15)

    lpswe   LC_RETURN_PSW(0)

    .size   ret_to_user, . - ret_to_user
    .section .note.GNU-stack, "", @progbits
