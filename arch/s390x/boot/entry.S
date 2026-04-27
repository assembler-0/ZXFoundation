/* SPDX-License-Identifier: Apache-2.0 */
/* arch/s390x/boot/entry.S - ZXFoundation bootstrap */

    .section .text
    .globl _start
    .type _start, @function

_start:
    larl    %r15, stack_top
    aghi    %r15, -160
    lghi    %r11, 0

    brasl   %r14, zxfoundation_global_initialize

halt:
    larl    %r1, disabled_wait
    lpswe   0(%r1)

.align 8
disabled_wait:
    .quad 0x0002000180000000, 0x0000000000000000 /* Disabled wait PSW */

.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:
