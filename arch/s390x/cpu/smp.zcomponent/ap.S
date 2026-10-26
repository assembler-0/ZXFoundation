// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/smp.zcomponent/ap.S

#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/init/zxfl/psw.h>

    .section .text.ap_entry, "ax", @progbits
    .globl ap_entry
ap_entry:
    lg      %r15, LC_RESTART_STACK(0)
    aghi    %r15, -160

    lctlg   %c0, %c0, LC_AP_CR0(0)
    lctlg   %c1, %c1, LC_KERNEL_ASCE(0)
    lctlg   %c13, %c13, LC_AP_CR13(0)

    lpswe   LC_RETURN_PSW(0)

    .globl ap_dat_on
ap_dat_on:
    lg      %r15, LC_KERNEL_STACK(0)
    aghi    %r15, -160

    sgr     %r0, %r0
    stg     %r0, 0(%r15)
    stg     %r0, 112(%r15)

    brasl   %r14, ap_startup

.Lap_halt:
    larl    %r1, .Lap_disabled_wait
    lpswe   0(%r1)
    j       .Lap_halt

    .align 8
.Lap_disabled_wait:
    .quad   PSW_MASK_DISABLED_WAIT, 0x0000000000DEAD07

    .section .note.GNU-stack, "", @progbits
