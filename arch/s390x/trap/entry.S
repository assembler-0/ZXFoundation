/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/trap/entry.S
/// @brief Low-level interrupt entry stubs for z/Architecture.

#include <arch/s390x/cpu/irq_frame.h>
#include <arch/s390x/init/zxfl/psw.h>
#include <arch/s390x/cpu/lowcore.h>

#define DOMAIN_FLAGS_OFF  32  // domain.flags after kobject[24] and name[8].
#define TIF_NEED_RESCHED 4

// SAVE_FRAME — save interrupt register state on the async stack.
.macro SAVE_FRAME reg, old_psw_lc, stack_lc, irq_class_val
    stg     \reg, LC_SCRATCH0(0)
    icm     \reg, 15, LC_IRQ_NESTING(0)
    jnz     1f
    lg      \reg, \stack_lc(0)
    .ifnc \stack_lc, LC_MCCK_STACK
    ltgr    \reg, \reg
    jnz     2f
    lg      \reg, LC_KERNEL_STACK(0)
2:
    .endif
    j       3f
1:
    lgr     \reg, %r15
3:
    aghi    \reg, -(IRQ_FRAME_BIAS + IRQ_FRAME_SIZE)
    stmg    %r0, %r15, (IRQ_FRAME_BIAS + IRQ_FRAME_GPRS)(\reg)
    mvc     (IRQ_FRAME_BIAS + IRQ_FRAME_PSW_MASK)(16, \reg), \old_psw_lc(0)
    stam    %a0, %a15, (IRQ_FRAME_BIAS + IRQ_FRAME_ARS)(\reg)
    stfpc   (IRQ_FRAME_BIAS + IRQ_FRAME_FPC)(\reg)
    vstm    %v0, %v15, (IRQ_FRAME_BIAS + IRQ_FRAME_VRS)(\reg)
    vstm    %v16, %v31, (IRQ_FRAME_BIAS + IRQ_FRAME_VRS + 256)(\reg)
    lgr     %r15, \reg
    // Restore the original scratch-register value into the saved frame so
    // that RESTORE_FRAME gives back the correct register contents.
    lg      \reg, LC_SCRATCH0(0)
    stg     \reg, (IRQ_FRAME_BIAS + IRQ_FRAME_GPRS + 8)(%r15)
    asi     LC_IRQ_NESTING(0), 1
    la      %r2, IRQ_FRAME_BIAS(%r15)
    lhi     \reg, \irq_class_val
    stc     \reg, LC_IRQ_CLASS(0)
.endm

.macro RESTORE_FRAME
    vlm     %v0, %v15, (IRQ_FRAME_BIAS + IRQ_FRAME_VRS)(%r15)
    vlm     %v16, %v31, (IRQ_FRAME_BIAS + IRQ_FRAME_VRS + 256)(%r15)
    lfpc    (IRQ_FRAME_BIAS + IRQ_FRAME_FPC)(%r15)
    lam     %a0, %a15, (IRQ_FRAME_BIAS + IRQ_FRAME_ARS)(%r15)
    mvc     LC_PSW_TEMP(16, 0), (IRQ_FRAME_BIAS + IRQ_FRAME_PSW_MASK)(%r15)
    asi     LC_IRQ_NESTING(0), -1
    lhi     %r0, 0
    stc     %r0, LC_IRQ_CLASS(0)
    lmg     %r0, %r15, (IRQ_FRAME_BIAS + IRQ_FRAME_GPRS)(%r15)
    lpswe   LC_PSW_TEMP(0)
.endm

.macro SAVE_NUCLEUS_FRAME reg, old_psw_lc, stack_lc, irq_class_val
    stg     \reg, LC_SCRATCH0(0)
    tm      (\old_psw_lc + 1)(0), 0x01
    jz      1f
    xgr     \reg, \reg
    st      \reg, LC_IRQ_NESTING(0)
    st      \reg, LC_NUCLEUS_NESTING(0)
1:
    icm     \reg, 15, LC_NUCLEUS_NESTING(0)
    jnz     2f
    lg      \reg, \stack_lc(0)
    j       3f
2:
    lgr     \reg, %r15
3:
    aghi    \reg, -(IRQ_FRAME_BIAS + IRQ_FRAME_SIZE)
    stmg    %r0, %r15, (IRQ_FRAME_BIAS + IRQ_FRAME_GPRS)(\reg)
    mvc     (IRQ_FRAME_BIAS + IRQ_FRAME_PSW_MASK)(16, \reg), \old_psw_lc(0)
    stam    %a0, %a15, (IRQ_FRAME_BIAS + IRQ_FRAME_ARS)(\reg)
    stfpc   (IRQ_FRAME_BIAS + IRQ_FRAME_FPC)(\reg)
    vstm    %v0, %v15, (IRQ_FRAME_BIAS + IRQ_FRAME_VRS)(\reg)
    vstm    %v16, %v31, (IRQ_FRAME_BIAS + IRQ_FRAME_VRS + 256)(\reg)
    lgr     %r15, \reg
    lg      \reg, LC_SCRATCH0(0)
    stg     \reg, (IRQ_FRAME_BIAS + IRQ_FRAME_GPRS + 8)(%r15)
    asi     LC_NUCLEUS_NESTING(0), 1
    la      %r2, IRQ_FRAME_BIAS(%r15)
    lhi     \reg, \irq_class_val
    stc     \reg, LC_IRQ_CLASS(0)
.endm

.macro RESTORE_NUCLEUS_FRAME
    vlm     %v0, %v15, (IRQ_FRAME_BIAS + IRQ_FRAME_VRS)(%r15)
    vlm     %v16, %v31, (IRQ_FRAME_BIAS + IRQ_FRAME_VRS + 256)(%r15)
    lfpc    (IRQ_FRAME_BIAS + IRQ_FRAME_FPC)(%r15)
    lam     %a0, %a15, (IRQ_FRAME_BIAS + IRQ_FRAME_ARS)(%r15)
    mvc     LC_PSW_TEMP(16, 0), (IRQ_FRAME_BIAS + IRQ_FRAME_PSW_MASK)(%r15)
    asi     LC_NUCLEUS_NESTING(0), -1
    lhi     %r0, 0
    stc     %r0, LC_IRQ_CLASS(0)
    lmg     %r0, %r15, (IRQ_FRAME_BIAS + IRQ_FRAME_GPRS)(%r15)
    lpswe   LC_PSW_TEMP(0)
.endm

/// @brief Check for preemption after a preemptible interrupt.
.macro CHECK_PREEMPT
    tm      (IRQ_FRAME_BIAS + IRQ_FRAME_PSW_MASK + 1)(%r15), 0x01
    jz      1f

    l       %r1, LC_PREEMPT_COUNT(0)
    ltr     %r1, %r1
    jnz     1f

    lg      %r1, LC_CURRENT_DOMAIN(0)
    ltgr    %r1, %r1
    jz      1f
    l       %r1, DOMAIN_FLAGS_OFF(%r1)
    tmll    %r1, TIF_NEED_RESCHED
    jz      1f

    lg      %r1, LC_KERNEL_STACK(0)
    aghi    %r1, -IRQ_FRAME_BIAS
    stg     %r15, 0(%r1)            // backchain = interrupt frame pointer
    lgr     %r15, %r1               // r15 = kernel stack frame

    brasl   %r14, _ZN12zxfoundation5schedW12zxfoundationW5schedW5sched8scheduleEv

    lg      %r15, 0(%r15)
1:
.endm

    .section .text.trap_pgm, "ax", @progbits
    .globl  trap_pgm_entry
trap_pgm_entry:
    // irq_class = 3 (pgm)
    SAVE_FRAME %r1, LC_PGM_OLD_PSW, LC_ASYNC_STACK, 3
    brasl   %r14, do_pgm_interrupt
    CHECK_PREEMPT
    RESTORE_FRAME

    .section .text.trap_ext, "ax", @progbits
    .globl  trap_ext_entry
trap_ext_entry:
    // irq_class = 1 (ext)
    SAVE_FRAME %r1, LC_EXT_OLD_PSW, LC_ASYNC_STACK, 1
    brasl   %r14, do_ext_interrupt
    CHECK_PREEMPT
    RESTORE_FRAME

    .section .text.trap_io, "ax", @progbits
    .globl  trap_io_entry
trap_io_entry:
    // irq_class = 2 (io)
    SAVE_FRAME %r1, LC_IO_OLD_PSW, LC_ASYNC_STACK, 2
    brasl   %r14, do_io_interrupt
    CHECK_PREEMPT
    RESTORE_FRAME

    .section .text.trap_svc, "ax", @progbits
    .globl  trap_svc_entry
trap_svc_entry:
    // irq_class = 4 (svc) — nucleus call dispatch (was the PC gate).
    SAVE_NUCLEUS_FRAME %r1, LC_SVC_OLD_PSW, LC_KERNEL_STACK, 4
    llgh    %r3, 0x008a(0)          // %r3 = svc_code (u16) from lowcore PSA+0x8A
    brasl   %r14, do_svc_interrupt // C handler: dispatch nucleus call
    CHECK_PREEMPT
    RESTORE_NUCLEUS_FRAME

    .section .text.trap_mcck, "ax", @progbits
    .globl  trap_mcck_entry
trap_mcck_entry:
    // irq_class = 5 (mcck)
    SAVE_FRAME %r1, LC_MCCK_OLD_PSW, LC_MCCK_STACK, 5
    brasl   %r14, do_mcck_interrupt
    RESTORE_FRAME

    .section .note.GNU-stack, "", @progbits
