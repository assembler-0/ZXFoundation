// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/entry.S - ZXFoundation Kernel Entry Point

#include <zxfoundation/zconfig.h>

    .machinemode zarch
    .section .text.__zx_start_kernel64, "ax", @progbits
    .globl __zx_start_kernel64

__zx_start_kernel64:
    larl    %r15, stack_top
    aghi    %r15, -CONFIG_S390X_SAVE_AREA

    brasl   %r14, zxfoundation_global_initialize

.Lhalt:
    larl    %r1, disabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt

disabled_wait_psw:
    .quad   CONFIG_PSW_DISABLED_WAIT, 0x0000000000000000

    .section .bss
    .align 16
stack_bottom:
    .skip   CONFIG_BOOT_STACK_SIZE
stack_top:

    .section .note.GNU-stack, "", @progbits
