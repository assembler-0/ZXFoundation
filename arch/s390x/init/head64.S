// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/head64.S

#include <zxfoundation/zconfig.h>

    .machinemode zarch

// ---------------------------------------------------------------------------
// Handshake stub - Identity Mapped
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Kernel Entry Point - Virtual Mode (DAT already enabled by loader)
// ---------------------------------------------------------------------------
    .section .text.__zx_start_kernel64, "ax", @progbits
    .globl __zx_start_kernel64
__zx_start_kernel64:
    // Load stack pointer from boot protocol (R2 points to s_proto)
    // kernel_stack_top is at offset 392
    lg      %r15, 392(%r2)
    aghi    %r15, -160
    
    brasl   %r14, zxfoundation_global_initialize

.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt

    .align 16
.Ldisabled_wait_psw:
    .quad   0x0000800180000000, 0x0000000000000000

    .section .boot.text, "ax"
    .align 16
.Lboot_stack:
    .skip 8192
.Lboot_stack_top:

    .section .note.GNU-stack, "", @progbits
