// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/head64.S

#include <zxfoundation/zconfig.h>
#include <arch/s390x/init/zxfl/zxvl_private.h>
#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/cpu/psw.h>

    .section .text.handshake, "ax"
    .globl __zxfl_handshake_stub

__zxfl_handshake_stub:
    llihf   %r0, ZXVL_SEED_HI
    iilf    %r0, ZXVL_SEED_LO
    xgr     %r2, %r0
    lgr     %r0, %r2
    sllg    %r0, %r0, 17
    srlg    %r1, %r2, 47
    ogr     %r0, %r1
    llihf   %r1, ZXVL_HS_RESPONSE_HI
    iilf    %r1, ZXVL_HS_RESPONSE_LO
    lgr     %r2, %r0
    agr     %r2, %r1
    br      %r14

    .section .text.__zx_start_kernel64, "ax", @progbits
    .globl __zx_start_kernel64
__zx_start_kernel64:
    lgr     %r11, %r2

    larl    %r2, __bss_start
    larl    %r3, __bss_end
    sgr     %r3, %r2                       // R3 = BSS size in bytes
    jz      .Lbss_done
    lgr     %r4, %r2                       // R4 = source (same as dest)
    sgr     %r5, %r5                       // R5 = 0 (source length 0 + pad byte 0x00)
.Lmvcl_retry:
    mvcl    %r2, %r4
    jo      .Lmvcl_retry
.Lbss_done:

    lgr     %r2, %r11
    lg      %r15, 392(%r2)                 // kernel_stack_top from boot protocol
    aghi    %r15, -160                     // 160-byte register save area

    brasl   %r14, zx_lowcore_setup_pre_dat

    lgr     %r2, %r11
    brasl   %r14, zxfoundation_global_initialize

.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt

    .align 8
.Ldisabled_wait_psw:
    .quad   PSW_MASK_DISABLED_WAIT, 0x0000000000000000

    .section .text.ap_entry, "ax", @progbits
    .globl ap_entry
ap_entry:
    lg      %r15, LC_RESTART_STACK(0)
    aghi    %r15, -160

    lctlg   %c1, %c1, LC_KERNEL_ASCE(0)

    lpswe   LC_RETURN_PSW(0)

    .globl ap_dat_on
ap_dat_on:
    lg      %r15, LC_KERNEL_STACK(0)
    aghi    %r15, -160
    brasl   %r14, ap_startup

.Lap_halt:
    larl    %r1, .Lap_disabled_wait
    lpswe   0(%r1)
    j       .Lap_halt

    .align 8
.Lap_disabled_wait:
    .quad   PSW_MASK_DISABLED_WAIT, 0x0000000000DEAD07

    .section .note.GNU-stack, "", @progbits
