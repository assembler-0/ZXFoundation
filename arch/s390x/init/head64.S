// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/head64.S

#include <zxfoundation/zconfig.h>

// Handshake constants — must match zxfl_private.h exactly.
// Split into HI/LO halves because GAS can't handle 64-bit immediates directly.
// ZXFL_HS_CHALLENGE = 0xFEEDFACECAFEBABE
// ZXFL_HS_RESPONSE  = 0xDEADBEEF0BADF00D
.equ HS_CHALLENGE_HI, 0xFEEDFACE
.equ HS_CHALLENGE_LO, 0xCAFEBABE
.equ HS_RESPONSE_HI,  0xDEADBEEF
.equ HS_RESPONSE_LO,  0x0BADF00D

    .machinemode zarch

// ---------------------------------------------------------------------------
// Handshake stub — placed at kernel_base + 0x200 by the linker script.
// The loader calls this before jumping to the real entry point.
//
// Protocol (simplified — no binding_token in asm, that's verified in C):
//   Loader sends: R2 = HS_CHALLENGE
//   Stub returns: R2 = HS_RESPONSE
//
// The loader verifies R2 == HS_RESPONSE. An outsider's kernel has no stub
// at this secret offset and will return garbage or crash.
// ---------------------------------------------------------------------------
    .section .text.handshake, "ax"
    .globl __zxfl_handshake_stub
// ZXFL_SEED split (must match zxfl_private.h)
// ZXFL_SEED = 0xA5F0C3E1B2D49687
.equ ZXFL_SEED_HI, 0xA5F0C3E1
.equ ZXFL_SEED_LO, 0xB2D49687

__zxfl_handshake_stub:
    // Input:  R2 = ZXFL_SEED ^ nonce  (challenge from loader)
    // Output: R2 = rotl64(nonce, 17) + ZXFL_HS_RESPONSE
    //
    // Step 1: recover nonce = challenge ^ ZXFL_SEED
    llihf   %r0, ZXFL_SEED_HI
    iilf    %r0, ZXFL_SEED_LO
    xgr     %r2, %r0                    // r2 = nonce

    // Step 2: rotl64(nonce, 17) = (nonce << 17) | (nonce >> 47)
    lgr     %r0, %r2
    sllg    %r0, %r0, 17                // r0 = nonce << 17
    srlg    %r1, %r2, 47                // r1 = nonce >> 47
    ogr     %r0, %r1                    // r0 = rotl64(nonce, 17)

    // Step 3: add ZXFL_HS_RESPONSE
    llihf   %r1, HS_RESPONSE_HI
    iilf    %r1, HS_RESPONSE_LO
    agr     %r2, %r1                    // r2 = rotl64(nonce,17) + HS_RESPONSE
    // Wait — we need rotl result, not nonce. Fix:
    lgr     %r2, %r0
    agr     %r2, %r1                    // r2 = rotl64(nonce,17) + HS_RESPONSE
    br      %r14

.Lhs_fail:
    larl    %r1, .Lhs_halt_psw
    lpswe   0(%r1)
    .align 8
.Lhs_halt_psw:
    .quad   CONFIG_PSW_DISABLED_WAIT, 0x0000000000000000

// ---------------------------------------------------------------------------
// Real kernel entry point
// ---------------------------------------------------------------------------
    .section .text.__zx_start_kernel64, "ax", @progbits
    .globl __zx_start_kernel64
__zx_start_kernel64:
    lgr     %r11, %r2                   // preserve boot protocol pointer

    // Zero BSS
    larl    %r2, __bss_start
    larl    %r3, __bss_end
    sgr     %r3, %r2
    jz      .Lbss_done
    lgr     %r4, %r2
    sgr     %r5, %r5
.Lmvcl_retry:
    mvcl    %r2, %r4
    jo      .Lmvcl_retry
.Lbss_done:

    // Switch to loader-provided stack (C helper extracts it from protocol)
    lgr     %r2, %r11
    brasl   %r14, zx_get_loader_stack_top
    ltgr    %r2, %r2
    jz      .Luse_bss_stack
    lgr     %r15, %r2
    aghi    %r15, -CONFIG_S390X_SAVE_AREA
    j       .Lstack_ready
.Luse_bss_stack:
    larl    %r15, stack_top
    aghi    %r15, -CONFIG_S390X_SAVE_AREA
.Lstack_ready:

    lgr     %r2, %r11
    brasl   %r14, zxfoundation_global_initialize

.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt

    .align 8
.Ldisabled_wait_psw:
    .quad   CONFIG_PSW_DISABLED_WAIT, 0x0000000000000000

    .section .bss
    .align 16
stack_bottom:
    .skip   CONFIG_BOOT_STACK_SIZE
stack_top:

    .section .note.GNU-stack, "", @progbits
