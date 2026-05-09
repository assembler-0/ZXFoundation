// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage1/head.S

#include <arch/s390x/cpu/psw.h>

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
    la      %r7, 2                      // order: z/Arch + restore 64-bit PSWs
    la      %r6, 0                      // target CPU address = 0
    sigp    %r7, %r6, 0x12             // SIGP SET ARCHITECTURE
    bc      8, .Lswitched64            // branch if accepted (CC=0)
    la      %r7, 1                      // fallback: z/Arch without PSW restore
    sigp    %r7, %r6, 0x12
.Lswitched64:
    sam64                               // switch addressing mode to 64-bit

    larl    %r2, __bss_start
    larl    %r3, __bss_stop
    sgr     %r3, %r2                    // r3 = BSS length
    jz      .Lbss_done
.Lbss_loop:
    aghi    %r3, -1
    mvi     0(%r2), 0
    la      %r2, 1(%r2)
    brctg   %r3, .Lbss_loop
.Lbss_done:

    larl    %r15, stage1_stack_top
    aghi    %r15, -160

    llgf    %r2, 0xb8                   // load schid from lowcore (31-bit zero-extend)

    brasl   %r14, zxfl00_entry

.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt                      // unreachable; silences assembler warning

    .align 8
.Ldisabled_wait_psw:
    .quad   PSW_MASK_DISABLED_WAIT      // disabled-wait, 64-bit (bit 14 = wait)
    .quad   0x00000000DEAD0001          // halt address (operator-visible)

    .section .bss
    .align 16
stage1_stack_bottom:
    .skip   8192
stage1_stack_top:

    .section .note.GNU-stack, "", @progbits
