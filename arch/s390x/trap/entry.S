/// SPDX-License-Identifier: Apache-2.0
/// @file entry.S
/// @brief Low-level interrupt entry stubs for z/Architecture.

#include <arch/s390x/cpu/irq_frame.h>
#include <arch/s390x/init/zxfl/psw.h>
#include <arch/s390x/cpu/lowcore.h>

// TODO: hook up the interrupts

.macro SAVE_FRAME reg, old_psw_lc, stack_lc
    lg      \reg, \stack_lc(0)
    aghi    \reg, -(160 + IRQ_FRAME_SIZE)
    stmg    %r0, %r15, (160 + IRQ_FRAME_GPRS)(\reg)
    mvc     (160 + IRQ_FRAME_PSW_MASK)(16, \reg), \old_psw_lc(0)
    lgr     %r15, \reg
    la      %r2, 160(%r15)
.endm

.macro RESTORE_FRAME
    lmg     %r0, %r14, (160 + IRQ_FRAME_GPRS)(%r15)
    lpswe   (160 + IRQ_FRAME_PSW_MASK)(%r15)
.endm

    .section .text.trap_pgm, "ax", @progbits
    .globl  trap_pgm_entry
trap_pgm_entry:
    SAVE_FRAME %r1, LC_PGM_OLD_PSW, LC_ASYNC_STACK
    //brasl   %r14, do_pgm_check
    RESTORE_FRAME

    .section .text.trap_ext, "ax", @progbits
    .globl  trap_ext_entry
trap_ext_entry:
    SAVE_FRAME %r1, LC_EXT_OLD_PSW, LC_ASYNC_STACK
    //brasl   %r14, do_ext_interrupt
    RESTORE_FRAME

    .section .text.trap_io, "ax", @progbits
    .globl  trap_io_entry
trap_io_entry:
    SAVE_FRAME %r1, LC_IO_OLD_PSW, LC_ASYNC_STACK
    //brasl   %r14, do_io_interrupt
    RESTORE_FRAME

    .section .text.trap_mcck, "ax", @progbits
    .globl  trap_mcck_entry
trap_mcck_entry:
    SAVE_FRAME %r1, LC_MCCK_OLD_PSW, LC_MCCK_STACK
    //brasl   %r14, do_mcck_interrupt
    RESTORE_FRAME

    .section .note.GNU-stack, "", @progbits