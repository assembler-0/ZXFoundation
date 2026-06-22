/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/sched/switch_to.S
/// @brief Context switch — save/restore callee-saved GPRs and swap kernel stacks.

#include <arch/s390x/cpu/lowcore.h>

#define STRAND_OWNER        24   ///< strand::owner (void* — back-pointer to domain)
#define STRAND_KSTACK_TOP   112  ///< strand::kstack_top (verified by static_assert)
#define STRAND_KSP          120  ///< strand::ksp        (verified by static_assert)
#define SF_GPRS             48
#define LC_CURRENT_DOMAIN   0x0340

    .section .text, "ax", @progbits
    .globl  __switch_to_asm
    .type   __switch_to_asm, @function
    .align  8

/// @brief Low-level context switch (strand-aware).
///   %r2 = strand* prev
///   %r3 = strand* next
/// Returns:
///   %r2 = prev's owning domain (for the caller to finish domain teardown)
__switch_to_asm:
    stmg    %r6, %r15, SF_GPRS(%r15)
    aghi    %r15, -128
    vstm    %v8, %v15, 0(%r15)
    stg     %r15, STRAND_KSP(%r2)           // save prev's ksp (strand.ksp)

    // Update lowcore: current_domain = next->owner
    lg      %r4, STRAND_OWNER(%r3)
    stg     %r4, LC_CURRENT_DOMAIN(0)

    // Update kernel stack top in lowcore from next strand
    lg      %r5, STRAND_KSTACK_TOP(%r3)
    stg     %r5, LC_KERNEL_STACK(0)

    // Load next's kernel stack pointer
    lg      %r15, STRAND_KSP(%r3)

    // Return the PREVIOUS domain pointer (prev->owner) in %r2
    lg      %r2, STRAND_OWNER(%r2)

    vlm     %v8, %v15, 0(%r15)
    aghi    %r15, 128
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

    lpswe   LC_RETURN_PSW(0)

    .size   domain_trampoline, . - domain_trampoline
    .section .note.GNU-stack, "", @progbits
