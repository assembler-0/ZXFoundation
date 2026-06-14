/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/trap/entry.S
/// @brief Low-level interrupt entry stubs for z/Architecture.
///
/// ── Preemption check ────────────────────────────────────────────────────
///
///   After EXT/IO interrupt handlers return, we check:
///     1. preempt_count == 0  (not in a critical section)
///     2. task->flags has NEED_RESCHED set
///   If both, call schedule() before returning to the interrupted context.

#include <arch/s390x/cpu/irq_frame.h>
#include <arch/s390x/init/zxfl/psw.h>
#include <arch/s390x/cpu/lowcore.h>

/// LC_IRQ_NESTING = LC_PERCPU_OFFSET + offsetof(s390x_percpu_data, irq_nesting)
/// Verified at compile time by the static_assert in lowcore_types.cxxm.
#define LC_IRQ_NESTING  0x0430

/// LC_CURRENT_TASK = 0x0340 (zx_lowcore::current_task)
#define LC_CURRENT_TASK 0x0340

/// LC_PREEMPT_COUNT = LC_PERCPU_OFFSET + offsetof(s390x_percpu_data, preempt_count)
/// percpu starts at 0x0400. preempt_count is at offset 40 = 0x28 within percpu.
#define LC_PREEMPT_COUNT 0x0428

/// offsetof(task, flags) = 20
#define TASK_FLAGS_OFF  20

/// task_flags::need_resched = 1 << 3 = 8
#define TIF_NEED_RESCHED 8

.macro SAVE_FRAME reg, old_psw_lc, stack_lc
    lg      \reg, \stack_lc(0)
    aghi    \reg, -(160 + IRQ_FRAME_SIZE)
    stmg    %r0, %r15, (160 + IRQ_FRAME_GPRS)(\reg)
    mvc     (160 + IRQ_FRAME_PSW_MASK)(16, \reg), \old_psw_lc(0)
    lgr     %r15, \reg
    asi     LC_IRQ_NESTING(0), 1
    la      %r2, 160(%r15)
.endm

.macro RESTORE_FRAME
    lmg     %r0, %r14, (160 + IRQ_FRAME_GPRS)(%r15)
    asi     LC_IRQ_NESTING(0), -1
    lpswe   (160 + IRQ_FRAME_PSW_MASK)(%r15)
.endm

/// @brief Check for preemption after a preemptible interrupt.
///
/// If preempt_count == 0 and current_task->flags has NEED_RESCHED,
/// call schedule() to preempt the interrupted context.
.macro CHECK_PREEMPT
    // Check preempt_count == 0
    l       %r1, LC_PREEMPT_COUNT(0)
    ltr     %r1, %r1
    jnz     .Lno_preempt\@

    // Check need_resched in current task flags
    lg      %r1, LC_CURRENT_TASK(0)
    ltgr    %r1, %r1
    jz      .Lno_preempt\@
    l       %r1, TASK_FLAGS_OFF(%r1)
    tmll    %r1, TIF_NEED_RESCHED
    jz      .Lno_preempt\@

    // Preempt: call schedule() with IRQs disabled (schedule handles it)
    brasl   %r14, _ZN12zxfoundation5schedW12zxfoundationW5schedW5sched8scheduleEv
.Lno_preempt\@:
.endm

    .section .text.trap_pgm, "ax", @progbits
    .globl  trap_pgm_entry
trap_pgm_entry:
    SAVE_FRAME %r1, LC_PGM_OLD_PSW, LC_ASYNC_STACK
    brasl   %r14, do_pgm_check
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
    SAVE_FRAME %r1, LC_SVC_OLD_PSW, LC_KERNEL_STACK
    llgh    %r3, 0x008a(0)          /* load svc_code (u16) from lowcore */
    brasl   %r14, do_svc
    CHECK_PREEMPT
    RESTORE_FRAME

    .section .text.trap_mcck, "ax", @progbits
    .globl  trap_mcck_entry
trap_mcck_entry:
    SAVE_FRAME %r1, LC_MCCK_OLD_PSW, LC_MCCK_STACK
    brasl   %r14, do_mcck_interrupt
    RESTORE_FRAME

    .section .note.GNU-stack, "", @progbits
