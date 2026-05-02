// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage2/entry.S

    .machinemode zarch
    .section .text, "ax"
    .globl stage2_entry
stage2_entry:
    lgr     %r13, %r2

    larl    %r1, .Ldisabled_wait_psw
    mvc     0x080(16,%r0),  0(%r1)      // external new PSW
    mvc     0x090(16,%r0),  0(%r1)      // SVC new PSW
    mvc     0x0A0(16,%r0),  0(%r1)      // program-check new PSW
    mvc     0x0B0(16,%r0),  0(%r1)      // machine-check new PSW
    mvc     0x0C0(16,%r0),  0(%r1)      // I/O new PSW

    larl    %r1, .Lzero_byte
    ssm     0(%r1)

    larl    %r2, __bss_start
    larl    %r3, __bss_stop
    sgr     %r3, %r2
    jz      .Lbss_done
    lgr     %r4, %r2
    sgr     %r5, %r5
.Lmvcl_retry:
    mvcl    %r2, %r4
    jo      .Lmvcl_retry
.Lbss_done:

    larl    %r15, stage2_stack_top
    aghi    %r15, -160

    lgr     %r2, %r13

    brasl   %r14, zxfl01_entry

.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt

    .align 8
.Ldisabled_wait_psw:
    .quad   0x0000800180000000
    .quad   0x0000000000000000          // halt at 0x0 (always mapped)

.Lzero_byte:
    .byte   0x00
    .align  8

    .section .bss
    .align 16
stage2_stack_bottom:
    .skip   32768
stage2_stack_top:

    .section .note.GNU-stack, "", @progbits
