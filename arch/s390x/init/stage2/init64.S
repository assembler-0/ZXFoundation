// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/init64.S - ZXFoundation bootstrap for s390x

#include <zxfoundation/zconfig.h>

    .machinemode zarch
    .section .text.__zx_start_kernel64, "ax", @progbits
    .globl __zx_start_kernel64
__zx_start_kernel64:
    larl    %r15, stack_top
    aghi    %r15, -CONFIG_S390X_SAVE_AREA
    lghi    %r11, 0

    larl    %r2, __bss_start
    larl    %r3, __bss_end
    lghi    %r0, 0
.Lbss_zero_loop:
    cgr     %r2, %r3
    jnl     .Lbss_zero_done
    stc     %r0, 0(%r2)
    aghi    %r2, 1
    j       .Lbss_zero_loop
.Lbss_zero_done:

    brasl   %r14, trap_init
    brasl   %r14, zxfoundation_global_initialize

.Lhalt:
    larl    %r1, disabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt

disabled_wait_psw:
    .quad   CONFIG_PSW_DISABLED_WAIT, 0x0000000000000000

    .section .bss
    .balign 16

stack_bottom:
    .skip   CONFIG_BOOT_STACK_SIZE
stack_top:

    .section .note.GNU-stack, "", @progbits