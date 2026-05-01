// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage2/entry.S - Stage 2 Entry (64-bit)

#include <zxfoundation/zconfig.h>

    .section .text, "ax"
    .globl stage2_entry
    .org 0x0
stage2_entry:
    /* R2 contains the subchannel ID from Stage 1 */
    
    /* Set up 64-bit stack */
    larl    %r15, stage2_stack_top
    aghi    %r15, -160                  /* Save area */

    /* Jump to C code */
    brasl   %r14, zxfl01_entry

.Lhalt:
    lpswe   .Ldisabled_wait_psw
    j       .Lhalt

    .align 16
.Ldisabled_wait_psw:
    .quad   0x0002000180000000          /* 64-bit, wait state */
    .quad   0x00000000DEADBEEF

    .section .bss
    .align 16
stage2_stack_bottom:
    .skip   32768                       /* Larger stack for stage 2 */
stage2_stack_top:

    .section .note.GNU-stack, "", @progbits
