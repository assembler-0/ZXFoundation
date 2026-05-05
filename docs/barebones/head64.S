// SPDX-License-Identifier: Apache-2.0

#define CONFIG_KERNEL_VIRT_OFFSET           0xFFFF800000000000ULL

    .machinemode zarch
    .section .text.handshake, "ax"
    .globl __zxfl_handshake_stub
.equ ZXFL_SEED_HI, 0xA5F0C3E1
.equ ZXFL_SEED_LO, 0xB2D49687
.equ HS_RESPONSE_HI,  0xDEADBEEF
.equ HS_RESPONSE_LO,  0x0BADF00D

__zxfl_handshake_stub:
    llihf   %r0, ZXFL_SEED_HI
    iilf    %r0, ZXFL_SEED_LO
    xgr     %r2, %r0
    lgr     %r0, %r2
    sllg    %r0, %r0, 17
    srlg    %r1, %r2, 47
    ogr     %r0, %r1
    llihf   %r1, HS_RESPONSE_HI
    iilf    %r1, HS_RESPONSE_LO
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

    // Restore boot protocol pointer and set up save area.
    lgr     %r2, %r11
    lg      %r15, 392(%r2)                 // kernel_stack_top from boot protocol
    aghi    %r15, -160                     // 160-byte register save area

    lgrl    %r0, .Lpgm_psw_mask            // R0 = PSW mask (disabled-wait, 64-bit)
    lgrl    %r1, .Lpgm_psw_addr            // R1 = PSW addr (recognizable halt)
    lgrl    %r3, .Lpgm_psw_slot            // R3 = HHDM virt addr of pgm_new_psw
    stg     %r0, 0(%r3)
    stg     %r1, 8(%r3)

    lgr     %r2, %r11
    brasl   %r14, kmain

.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt

    .align 8
.Ldisabled_wait_psw:
    .quad   0x0000800180000000, 0x0000000000000000

    .align 8
.Lpgm_psw_mask:
    .quad   0x0000800180000000
.Lpgm_psw_addr:
    .quad   (CONFIG_KERNEL_VIRT_OFFSET + 0xDEAD30)
.Lpgm_psw_slot:
    .quad   (CONFIG_KERNEL_VIRT_OFFSET + 0x0A0)

    .section .note.GNU-stack, "", @progbits
