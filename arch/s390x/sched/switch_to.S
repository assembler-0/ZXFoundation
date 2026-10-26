/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/sched/switch_to.S
/// @brief Context switch and first-entry trampolines for strands

#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/cpu/irq_frame.h>

#define STRAND_OWNER        24   ///< strand::owner (void* — back-pointer to domain)
#define STRAND_KSTACK_TOP   112  ///< strand::kstack_top 
#define STRAND_KSP          120  ///< strand::ksp        
#define STRAND_IRQ_NESTING     128  ///< strand::irq_nesting     
#define STRAND_NUCLEUS_NESTING 132  ///< strand::nucleus_nesting 
#define STRAND_IRQ_CLASS       136  ///< strand::irq_class       
#define SF_GPRS             48

#define FRAME_ARS           0
#define FRAME_FPC           64
#define FRAME_VRS           72
#define FRAME_SIZE          640

#define USER_ENTRY_SENTINEL  (-1)

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

    lg      %r4, LC_PERCPU_PTR(0)
    l       %r5, PCP_IRQ_NESTING(%r4)
    st      %r5, STRAND_IRQ_NESTING(%r2)
    l       %r5, PCP_NUCLEUS_NESTING(%r4)
    st      %r5, STRAND_NUCLEUS_NESTING(%r2)
    llc     %r5, PCP_IRQ_CLASS(%r4)
    stc     %r5, STRAND_IRQ_CLASS(%r2)

    lg      %r4, STRAND_OWNER(%r3)
    stg     %r4, LC_CURRENT_DOMAIN(0)

    lg      %r5, STRAND_KSTACK_TOP(%r3)
    stg     %r5, LC_KERNEL_STACK(0)

    lg      %r15, STRAND_KSP(%r3)

    lg      %r4, LC_PERCPU_PTR(0)
    l       %r5, STRAND_IRQ_NESTING(%r3)
    st      %r5, PCP_IRQ_NESTING(%r4)
    l       %r5, STRAND_NUCLEUS_NESTING(%r3)
    st      %r5, PCP_NUCLEUS_NESTING(%r4)
    llc     %r5, STRAND_IRQ_CLASS(%r3)
    stc     %r5, PCP_IRQ_CLASS(%r4)

    lg      %r2, STRAND_OWNER(%r2)          // return prev's domain for teardown

    vlm     %v0, %v15, FRAME_VRS(%r15)
    vlm     %v16, %v31, FRAME_VRS + 256(%r15)
    lfpc    FRAME_FPC(%r15)
    lam     %a0, %a15, FRAME_ARS(%r15)

    aghi    %r15, FRAME_SIZE
    lmg     %r6, %r15, SF_GPRS(%r15)
    br      %r14

    .size   __switch_to_asm, . - __switch_to_asm

/// @brief First-entry trampoline for kernel-thread strands.
///
///   r6  = arg    (void* passed to the entry function)
///   r7  = entry  (function pointer: void (*)(void*) noexcept)
///   r14 = &domain_trampoline (this function)
///
/// @details This trampoline runs the kernel entry function in supervisor (home)
///          space
    .globl  domain_trampoline
    .type   domain_trampoline, @function
    .align  8
domain_trampoline:
    stmg    %r6, %r15, SF_GPRS(%r15)    // save callee-saved to current frame
    lgr     %r1, %r15                    // back-chain
    aghi    %r15, -160                   // grow stack
    stg     %r1, 0(%r15)                 // store back-chain

    stosm   160(%r15), 0x03              // enable IO+EXT interrupts

    lgr     %r2, %r6                     // r2 = arg  (s390x ABI first arg)
    basr    %r14, %r7                    // call entry(arg)

    brasl   %r14, sched_strand_trampoline_exit
    j       .

    .size   domain_trampoline, . - domain_trampoline

/// @brief First-entry dispatcher for either a kernel-thread or a user-process
///        strand.
    .globl  ret_from_fork
    .type   ret_from_fork, @function
    .align  8
ret_from_fork:
    ltgr    %r7, %r7
    cghi    %r7, -1
    je      .Luser_first_switch
    j       .Lkthread_first_switch
.Luser_first_switch:
    mvc     LC_RETURN_PSW(16, 0), 0x110(%r15)    // copy user PSW mask + addr
    lmg     %r0, %r15, 0x90(%r15)               // load 16 user GPRs
    lpswe   LC_RETURN_PSW(0)
.Lkthread_first_switch:
    jg      domain_trampoline

    .size   ret_from_fork, . - ret_from_fork
    .section .note.GNU-stack, "", @progbits
