// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage2/entry.S

#include <arch/s390x/cpu/psw.h>

    .section .text, "ax"
    .globl stage2_entry
stage2_entry:
    lgr     %r13, %r2                   // save schid across calls

    brasl   %r14, zxfl_psw_install_safe_psws

    larl    %r1, .Lzero_byte
    ssm     0(%r1)                      // mask all interrupts off

    larl    %r2, __bss_start
    larl    %r3, __bss_stop
    sgr     %r3, %r2                    // r3 = BSS length
    jz      .Lbss_done
    lgr     %r4, %r2                    // r4 = source addr (unused; srclen=0)
    sgr     %r5, %r5                    // r5 = 0 (srclen=0, pad=0x00)
    mvcl    %r2, %r4                    // fill with pad byte 0x00
.Lbss_done:

    larl    %r15, stage2_stack_top
    aghi    %r15, -160

    lgr     %r2, %r13                   // restore schid into r2 (ABI arg0)

    brasl   %r14, zxfl01_entry

.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt

    .align 8
.Ldisabled_wait_psw:
    .quad   PSW_MASK_DISABLED_WAIT      // disabled-wait, 64-bit (bit 14 = wait)
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
