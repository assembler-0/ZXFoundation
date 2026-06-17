/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/sched/switch_to.S
/// @brief Context switch — save/restore callee-saved GPRs and swap kernel stacks.

#include <arch/s390x/cpu/lowcore.h>

#define DOMAIN_KSP          128
#define DOMAIN_KSTACK_TOP   120
#define SF_GPRS             48
#define LC_CURRENT_DOMAIN   0x0340

    .section .text, "ax", @progbits
    .globl  __switch_to_asm
    .type   __switch_to_asm, @function
    .align  8

/// @brief Low-level context switch.
///   %r2 = domain* prev
///   %r3 = domain* next
/// Returns:
///   %r2 = prev (unchanged, for the caller to finish_domain_switch)
__switch_to_asm:
    stmg    %r6, %r15, SF_GPRS(%r15)        
    stg     %r15, DOMAIN_KSP(%r2)           

    stg     %r3, LC_CURRENT_DOMAIN(0)       
    lg      %r4, DOMAIN_KSTACK_TOP(%r3)     
    stg     %r4, LC_KERNEL_STACK(0)         

    lg      %r15, DOMAIN_KSP(%r3)           
    lmg     %r6, %r15, SF_GPRS(%r15)        
    br      %r14                            

    .size   __switch_to_asm, . - __switch_to_asm

/// @brief First-entry trampoline for new domains.
///
/// __switch_to_asm restores callee-saved registers from the fake frame
/// built by setup_domain_context:
///   r6  = arg    (void* passed to the entry function)
///   r7  = entry  (function pointer: void (*)(void*) noexcept)
///   r14 = &domain_trampoline (this function)
///
/// The compiler's -ftrivial-auto-var-init=pattern WILL clobber r6/r7
/// in a C++ function prologue before any inline asm can read them.
/// This MUST be pure assembly.
    .globl  domain_trampoline
    .type   domain_trampoline, @function
    .align  8
domain_trampoline:
    // r6 = arg, r7 = entry — set by __switch_to_asm's lmg.

    // Allocate a standard s390x stack frame (160 bytes) so the entry
    // function has a valid back-chain and register save area.
    stmg    %r6, %r15, SF_GPRS(%r15)    // save callee-saved to current frame
    lgr     %r1, %r15                    // back-chain
    aghi    %r15, -160                   // grow stack
    stg     %r1, 0(%r15)                 // store back-chain

    // Enable external + I/O interrupts.
    // stosm stores old PSW byte at operand addr, then ORs mask into PSW.
    stosm   160(%r15), 0x03              // use padding byte in frame

    // Call entry(arg): r2 = first C argument.
    lgr     %r2, %r6
    basr    %r14, %r7

    // If entry() returns (shouldn't for [[noreturn]] entries), halt.
    lpswe   LC_RETURN_PSW(0)

    .size   domain_trampoline, . - domain_trampoline
    .section .note.GNU-stack, "", @progbits
