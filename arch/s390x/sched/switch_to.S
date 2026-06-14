/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/sched/switch_to.S
/// @brief Context switch — save/restore callee-saved GPRs and swap kernel stacks.

#include <arch/s390x/cpu/lowcore.h>

#define TASK_KSP            128
#define TASK_KSTACK_TOP     120
#define SF_GPRS             48
#define LC_CURRENT_TASK     0x0340

    .section .text, "ax", @progbits
    .globl  __switch_to_asm
    .type   __switch_to_asm, @function
    .align  8

/// @brief Low-level context switch.
///   %r2 = task* prev
///   %r3 = task* next
/// Returns:
///   %r2 = prev (unchanged, for the caller to finish_task_switch)
__switch_to_asm:
    stmg    %r6, %r15, SF_GPRS(%r15)        
    stg     %r15, TASK_KSP(%r2)             

    stg     %r3, LC_CURRENT_TASK(0)         
    lg      %r4, TASK_KSTACK_TOP(%r3)       
    stg     %r4, LC_KERNEL_STACK(0)         

    lg      %r15, TASK_KSP(%r3)             
    lmg     %r6, %r15, SF_GPRS(%r15)        
    br      %r14                            

    .size   __switch_to_asm, . - __switch_to_asm
    .section .note.GNU-stack, "", @progbits
