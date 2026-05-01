// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/zxfl_ipl.S - ZXFoundation Loader Entry

#include <zxfoundation/zconfig.h>

    .section .text.ipl, "ax"
    .globl __zx_ipl_entry
    .org 0x0

__zx_ipl_entry:
    .long   0x00080000
    .long   0x80000000 + zxfl_loader_entry

    .long   0x00000000, 0x00000000
    .long   0x00000000, 0x00000000

    .section .text, "ax"
    .globl zxfl_loader_entry
zxfl_loader_entry:
    larl    %r15, zxfl_stack_top
    ahi     %r15, -CONFIG_S390X_SAVE_AREA

    brasl   %r14, zxfl_main

.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpsw    0(%r1)
    j       .Lhalt

    .align 4
.Ldisabled_wait_psw:
    .long   0x000A0000
    .long   0xDEADDEAD

    .section .bss
    .align 16
zxfl_stack_bottom:
    .skip   CONFIG_BOOT_STACK_SIZE
zxfl_stack_top:

    .section .note.GNU-stack, "", @progbits
