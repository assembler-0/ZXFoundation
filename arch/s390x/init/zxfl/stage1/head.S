// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage1/head.S - Stage 1 Entry (IPL Record)

#include <zxfoundation/zconfig.h>

    .section .text.ipl, "ax"
    .globl __zx_ipl_entry
    .org 0x0

__zx_ipl_entry:
    .long   0x00080000
    .long   0x80000000 + stage1_entry

    .long   0x00000000, 0x00000000
    .long   0x00000000, 0x00000000

    .section .text, "ax"
    .globl stage1_entry
stage1_entry:
    larl    %r15, stage1_stack_top
    ahi     %r15, -160

    l       %r2, 0xb8
    
    brasl   %r14, zxfl00_entry

.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpsw    0(%r1)
    j       .Lhalt

    .align 8
.Ldisabled_wait_psw:
    .long   0x000A0000
    .long   0xDEADDEAD

    .section .bss
    .align 16
stage1_stack_bottom:
    .skip   4096
stage1_stack_top:

    .section .note.GNU-stack, "", @progbits
