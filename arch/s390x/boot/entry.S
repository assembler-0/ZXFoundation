// SPDX-License-Identifier: Apache-2.0
// arch/s390x/boot/entry.S - ZXFoundation bootstrap for s390x

#include <zxfoundation/zconfig.h>

#define PSW_ARCH_BITS       CONFIG_PSW_ARCH_BITS
#define PSW_DISABLED_WAIT   CONFIG_PSW_DISABLED_WAIT

    .section .text._start, "ax", @progbits
    .globl _start
    .type  _start, @function

_start:
    larl    %r1, initial_psw
    lpswe   0(%r1)

_start_64bit:

    larl    %r15, stack_top
    aghi    %r15, -CONFIG_S390X_SAVE_AREA
    lghi    %r11, 0

    larl    %r2, __bss_start
    larl    %r3, __bss_end
    lghi    %r0, 0

.Lbss_zero_loop:
    cgr     %r2, %r3                // if current >= end, done
    jnl     .Lbss_zero_done
    stc     %r0, 0(%r2)             // store zero byte
    aghi    %r2, 1
    j       .Lbss_zero_loop

.Lbss_zero_done:

    brasl   %r14, trap_init
    brasl   %r14, zxfoundation_global_initialize

.Lhalt:
    larl    %r1, disabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt
    .align 8

initial_psw:
    .quad   PSW_ARCH_BITS, _start_64bit

disabled_wait_psw:
    .quad   PSW_DISABLED_WAIT, 0x0000000000000000

    .section .bss
    .align 16

stack_bottom:
    .skip   CONFIG_BOOT_STACK_SIZE
stack_top:
