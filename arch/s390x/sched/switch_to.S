/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/sched/switch_to.S
/// @brief Context switch — save/restore callee-saved GPRs, all 32 VRs, ARs, FPC,
///        and swap kernel stacks.

#include <arch/s390x/cpu/lowcore.h>

#define STRAND_OWNER        24   ///< strand::owner (void* — back-pointer to domain)
#define STRAND_KSTACK_TOP   112  ///< strand::kstack_top (verified by static_assert)
#define STRAND_KSP          120  ///< strand::ksp        (verified by static_assert)
#define SF_GPRS             48

#define FRAME_ARS          0
#define FRAME_FPC          64
#define FRAME_VRS          72
#define FRAME_SIZE         640

    .section .text, "ax", @progbits
    .globl  __switch_to_asm
    .type   __switch_to_asm, @function
    .align  8

/// @brief Low-level context switch (strand-aware, full register save/restore).
///
/// Saves:    r6-r15, v0-v31, a0-a15, FPC
/// Restores: r6-r15, v0-v31, a0-a15, FPC
///
/// @param[in] %r2 = strand* prev
/// @param[in] %r3 = strand* next
/// @return    %r2 = prev's owning domain (for the caller to finish domain teardown)
__switch_to_asm:
    stmg    %r6, %r15, SF_GPRS(%r15)

    aghi    %r15, -FRAME_SIZE

    stam    %a0, %a15, FRAME_ARS(%r15)
    stfpc   FRAME_FPC(%r15)
    vstm    %v0, %v15, FRAME_VRS(%r15)
    vstm    %v16, %v31, FRAME_VRS + 256(%r15)

    stg     %r15, STRAND_KSP(%r2)           // save prev's ksp (strand.ksp)

    lg      %r4, STRAND_OWNER(%r3)
    stg     %r4, LC_CURRENT_DOMAIN(0)

    lg      %r5, STRAND_KSTACK_TOP(%r3)
    stg     %r5, LC_KERNEL_STACK(0)

    lg      %r15, STRAND_KSP(%r3)

    lg      %r2, STRAND_OWNER(%r2)          // return prev's domain for teardown

    vlm     %v0, %v15, FRAME_VRS(%r15)
    vlm     %v16, %v31, FRAME_VRS + 256(%r15)
    lfpc    FRAME_FPC(%r15)
    lam     %a0, %a15, FRAME_ARS(%r15)

    aghi    %r15, FRAME_SIZE
    lmg     %r6, %r15, SF_GPRS(%r15)
    br      %r14

    .size   __switch_to_asm, . - __switch_to_asm

/// @brief First-entry trampoline for new domains.
///   r6  = arg    (void* passed to the entry function)
///   r7  = entry  (function pointer: void (*)(void*) noexcept)
///   r14 = &domain_trampoline (this function)
    .globl  domain_trampoline
    .type   domain_trampoline, @function
    .align  8
domain_trampoline:
    stmg    %r6, %r15, SF_GPRS(%r15)    // save callee-saved to current frame
    lgr     %r1, %r15                    // back-chain
    aghi    %r15, -160                   // grow stack
    stg     %r1, 0(%r15)                 // store back-chain

    stosm   160(%r15), 0x03

    lgr     %r2, %r6
    basr    %r14, %r7

    brasl   %r14, sched_strand_trampoline_exit
    j       .

    .size   domain_trampoline, . - domain_trampoline
    .section .note.GNU-stack, "", @progbits
