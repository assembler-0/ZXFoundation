// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage2/entry.S

    .machinemode zarch
    .section .text, "ax"
    .globl stage2_entry
stage2_entry:
    lgr     %r13, %r2

    larl    %r2, __bss_start
    larl    %r3, __bss_stop
    sgr     %r3, %r2                    // r3 = BSS byte count
    jz      .Lbss_done
    lgr     %r4, %r2                    // src = dst (address irrelevant)
    sgr     %r5, %r5                    // srclen = 0, pad byte = 0
.Lmvcl_retry:
    mvcl    %r2, %r4                    // fill with pad (0) until dstlen exhausted
    jo      .Lmvcl_retry                // CC=1 = interrupted by channel, retry
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
    .quad   0x0002000180000000          // 64-bit disabled-wait PSW
    .quad   0x00000000DEAD0002          // halt indicator for stage 2

    .section .bss
    .align 16
stage2_stack_bottom:
    .skip   32768
stage2_stack_top:

    .section .note.GNU-stack, "", @progbits
