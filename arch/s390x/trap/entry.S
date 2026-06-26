/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/trap/entry.S
/// @brief Low-level interrupt entry stubs for z/Architecture.

#include <arch/s390x/cpu/irq_frame.h>
#include <arch/s390x/init/zxfl/psw.h>
#include <arch/s390x/cpu/lowcore.h>

#define DOMAIN_FLAGS_OFF  32  // domain.flags after kobject[24] and name[8].

#define TIF_NEED_RESCHED 4

.macro SAVE_FRAME reg, old_psw_lc, stack_lc
    icm     \reg, 15, LC_IRQ_NESTING(0)
    jnz     .Lnested\@
    lg      \reg, \stack_lc(0)
    j       .Lalloc_frame\@
.Lnested\@:
    lgr     \reg, %r15
.Lalloc_frame\@:
    aghi    \reg, -(160 + IRQ_FRAME_SIZE)
    stmg    %r0, %r15, (160 + IRQ_FRAME_GPRS)(\reg)
    mvc     (160 + IRQ_FRAME_PSW_MASK)(16, \reg), \old_psw_lc(0)
    stam    %a0, %a15, (160 + IRQ_FRAME_ARS)(\reg)
    stfpc   (160 + IRQ_FRAME_FPC)(\reg)
    vstm    %v0, %v15, (160 + IRQ_FRAME_VRS)(\reg)
    vstm    %v16, %v31, (160 + IRQ_FRAME_VRS + 256)(\reg)
    lgr     %r15, \reg
    asi     LC_IRQ_NESTING(0), 1
    la      %r2, 160(%r15)
.endm

.macro RESTORE_FRAME
    vlm     %v0, %v15, (160 + IRQ_FRAME_VRS)(%r15)
    vlm     %v16, %v31, (160 + IRQ_FRAME_VRS + 256)(%r15)
    lfpc    (160 + IRQ_FRAME_FPC)(%r15)
    lam     %a0, %a15, (160 + IRQ_FRAME_ARS)(%r15)
    mvc     LC_PSW_TEMP(16, 0), (160 + IRQ_FRAME_PSW_MASK)(%r15)
    lmg     %r0, %r15, (160 + IRQ_FRAME_GPRS)(%r15)
    asi     LC_IRQ_NESTING(0), -1
    lpswe   LC_PSW_TEMP(0)
.endm

.macro SAVE_NUCLEUS_FRAME reg, old_psw_lc, stack_lc
    tm      (\old_psw_lc + 1)(0), 0x01
    jz      .Lnucleus_keep_context\@
    xgr     \reg, \reg
    st      \reg, LC_IRQ_NESTING(0)
    st      \reg, LC_NUCLEUS_NESTING(0)
.Lnucleus_keep_context\@:
    icm     \reg, 15, LC_NUCLEUS_NESTING(0)
    jnz     .Lnucleus_nested\@
    lg      \reg, \stack_lc(0)
    j       .Lnucleus_alloc_frame\@
.Lnucleus_nested\@:
    lgr     \reg, %r15
.Lnucleus_alloc_frame\@:
    aghi    \reg, -(160 + IRQ_FRAME_SIZE)
    stmg    %r0, %r15, (160 + IRQ_FRAME_GPRS)(\reg)
    mvc     (160 + IRQ_FRAME_PSW_MASK)(16, \reg), \old_psw_lc(0)
    stam    %a0, %a15, (160 + IRQ_FRAME_ARS)(\reg)
    stfpc   (160 + IRQ_FRAME_FPC)(\reg)
    vstm    %v0, %v15, (160 + IRQ_FRAME_VRS)(\reg)
    vstm    %v16, %v31, (160 + IRQ_FRAME_VRS + 256)(\reg)
    lgr     %r15, \reg
    asi     LC_NUCLEUS_NESTING(0), 1
    la      %r2, 160(%r15)
.endm

.macro RESTORE_NUCLEUS_FRAME
    vlm     %v0, %v15, (160 + IRQ_FRAME_VRS)(%r15)
    vlm     %v16, %v31, (160 + IRQ_FRAME_VRS + 256)(%r15)
    lfpc    (160 + IRQ_FRAME_FPC)(%r15)
    lam     %a0, %a15, (160 + IRQ_FRAME_ARS)(%r15)
    mvc     LC_PSW_TEMP(16, 0), (160 + IRQ_FRAME_PSW_MASK)(%r15)
    lmg     %r0, %r15, (160 + IRQ_FRAME_GPRS)(%r15)
    asi     LC_NUCLEUS_NESTING(0), -1
    lpswe   LC_PSW_TEMP(0)
.endm

/// @brief Check for preemption after a preemptible interrupt.
.macro CHECK_PREEMPT
    tm      (160 + IRQ_FRAME_PSW_MASK + 1)(%r15), 0x01
    jz      .Lno_preempt\@

    l       %r1, LC_PREEMPT_COUNT(0)
    ltr     %r1, %r1
    jnz     .Lno_preempt\@

    lg      %r1, LC_CURRENT_DOMAIN(0)
    ltgr    %r1, %r1
    jz      .Lno_preempt\@
    l       %r1, DOMAIN_FLAGS_OFF(%r1)
    tmll    %r1, TIF_NEED_RESCHED
    jz      .Lno_preempt\@

    lg      %r1, LC_KERNEL_STACK(0)
    aghi    %r1, -160
    stg     %r15, 0(%r1)            // backchain = async stack pointer
    lgr     %r15, %r1               // r15 = kernel stack frame

    brasl   %r14, _ZN12zxfoundation5schedW12zxfoundationW5schedW5sched8scheduleEv

    lg      %r15, 0(%r15)
.Lno_preempt\@:
.endm

    .section .text.trap_pgm, "ax", @progbits
    .globl  trap_pgm_entry
trap_pgm_entry:
    SAVE_FRAME %r1, LC_PGM_OLD_PSW, LC_ASYNC_STACK
    brasl   %r14, do_pgm_check
    CHECK_PREEMPT
    RESTORE_FRAME

    .section .text.trap_ext, "ax", @progbits
    .globl  trap_ext_entry
trap_ext_entry:
    SAVE_FRAME %r1, LC_EXT_OLD_PSW, LC_ASYNC_STACK
    brasl   %r14, do_ext_interrupt
    CHECK_PREEMPT
    RESTORE_FRAME

    .section .text.trap_io, "ax", @progbits
    .globl  trap_io_entry
trap_io_entry:
    SAVE_FRAME %r1, LC_IO_OLD_PSW, LC_ASYNC_STACK
    brasl   %r14, do_io_interrupt
    CHECK_PREEMPT
    RESTORE_FRAME

    .section .text.trap_svc, "ax", @progbits
    .globl  trap_svc_entry
trap_svc_entry:
    SAVE_NUCLEUS_FRAME %r1, LC_SVC_OLD_PSW, LC_KERNEL_STACK
    llgh    %r3, 0x008a(0)          // Load svc_code (u16) from lowcore.
    brasl   %r14, do_svc
    RESTORE_NUCLEUS_FRAME

    .section .text.trap_mcck, "ax", @progbits
    .globl  trap_mcck_entry
trap_mcck_entry:
    SAVE_FRAME %r1, LC_MCCK_OLD_PSW, LC_MCCK_STACK
    brasl   %r14, do_mcck_interrupt
    RESTORE_FRAME

    .section .text.pc_entry, "ax", @progbits
    .globl  pc_gate_entry
    .type   pc_gate_entry, @function
    .align  8
pc_gate_entry:
    icm     %r1, 15, LC_IRQ_NESTING(0)
    jnz     .Lpc_nested
    icm     %r1, 15, LC_NUCLEUS_NESTING(0)
    jnz     .Lpc_nested
    lg      %r15, LC_KERNEL_STACK(0)
    j       .Lpc_alloc_frame
.Lpc_nested:
.Lpc_alloc_frame:
    aghi    %r15, -(160 + IRQ_FRAME_SIZE)

    stmg    %r0, %r15, (160 + IRQ_FRAME_GPRS)(%r15)
    stam    %a0, %a15, (160 + IRQ_FRAME_ARS)(%r15)
    stfpc   (160 + IRQ_FRAME_FPC)(%r15)
    vstm    %v0, %v15, (160 + IRQ_FRAME_VRS)(%r15)
    vstm    %v16, %v31, (160 + IRQ_FRAME_VRS + 256)(%r15)

    asi     LC_NUCLEUS_NESTING(0), 1

    lgr     %r7, %r6
    lgr     %r6, %r5
    lgr     %r5, %r4
    lgr     %r4, %r3
    lgr     %r3, %r2
    lgr     %r2, %r0

    brasl   %r14, do_nucleus_dispatch

    l       %r1, LC_PREEMPT_COUNT(0)
    ltr     %r1, %r1
    jnz     .Lpc_no_preempt

    lg      %r1, LC_CURRENT_DOMAIN(0)
    ltgr    %r1, %r1
    jz      .Lpc_no_preempt
    l       %r1, DOMAIN_FLAGS_OFF(%r1)
    tmll    %r1, TIF_NEED_RESCHED
    jz      .Lpc_no_preempt

    lg      %r1, LC_KERNEL_STACK(0)
    aghi    %r1, -160
    stg     %r15, 0(%r1)            // backchain = current frame pointer
    lgr     %r15, %r1
    brasl   %r14, _ZN12zxfoundation5schedW12zxfoundationW5schedW5sched8scheduleEv
    lg      %r15, 0(%r15)           // restore original frame pointer

.Lpc_no_preempt:
    vlm     %v0, %v15, (160 + IRQ_FRAME_VRS)(%r15)
    vlm     %v16, %v31, (160 + IRQ_FRAME_VRS + 256)(%r15)
    lfpc    (160 + IRQ_FRAME_FPC)(%r15)
    lam     %a0, %a15, (160 + IRQ_FRAME_ARS)(%r15)
    lmg     %r0, %r13, (160 + IRQ_FRAME_GPRS)(%r15)

    asi     LC_NUCLEUS_NESTING(0), -1

    .insn   e,0x0101                    // PR instruction
    .size   pc_gate_entry, . - pc_gate_entry

    .section .note.GNU-stack, "", @progbits
