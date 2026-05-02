// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/stage1/head.S
//
// Stage 1 IPL entry. The CPU starts in 31-bit ESA/390 mode on all z10+
// machines. We switch to 64-bit z/Architecture in the very first instructions
// before any C code runs, then call the 64-bit C loader.
//
// Binary layout (stage1.ld):
//   0x000: IPL PSW + CCW0 (8 bytes) — loaded by the channel subsystem
//   0x008: padding to 0x058
//   0x058: stage1_entry (this code)
//
// The IPL PSW at 0x0 is a 31-bit PSW that transfers control to stage1_entry.
// stage1_entry immediately switches to 64-bit mode, zeroes BSS, then calls
// zxfl00_entry(schid) compiled as a 64-bit translation unit.

    .machinemode zarch
    .section .text.ipl, "ax"
    .globl __zx_ipl_entry
    .org 0x0

__zx_ipl_entry:
    // 31-bit IPL PSW: EC=0, AM=31, addr=stage1_entry (bit 31 set = 31-bit)
    .long   0x00080000
    .long   0x80000000 + stage1_entry

    // Padding — the channel subsystem reads 24 bytes from the IPL record
    // before handing control to the PSW. Bytes 8-23 are unused CCW slots.
    .long   0x00000000, 0x00000000
    .long   0x00000000, 0x00000000

    .section .text, "ax"
    .globl stage1_entry
stage1_entry:
    // ----------------------------------------------------------------
    // Step 1: Switch to 64-bit z/Architecture mode.
    //
    // SIGP SET ARCHITECTURE (order code 0x12) with operand 2 requests
    // z/Architecture mode with 64-bit PSWs restored. We try code 2
    // first (z/Arch with 64-bit PSW restore), then fall back to code 1
    // (z/Arch without PSW restore) for older firmware variants.
    // R6=0 is the target CPU address (CPU 0 = ourselves).
    // ----------------------------------------------------------------
    la      %r7, 2                      // order: z/Arch + restore 64-bit PSWs
    la      %r6, 0                      // target CPU address = 0
    sigp    %r7, %r6, 0x12             // SIGP SET ARCHITECTURE
    bc      8, .Lswitched64            // branch if accepted (CC=0)
    la      %r7, 1                      // fallback: z/Arch without PSW restore
    sigp    %r7, %r6, 0x12
.Lswitched64:
    sam64                               // switch addressing mode to 64-bit

    // ----------------------------------------------------------------
    // Step 2: Zero BSS.
    //
    // The IPL record is loaded raw into memory; BSS is not zeroed by
    // the channel subsystem. Static variables in dasd_io.c (seek_arg,
    // search_arg, CCW chain) must start at zero.
    // ----------------------------------------------------------------
    larl    %r2, __bss_start
    larl    %r3, __bss_stop
    sgr     %r3, %r2                    // r3 = BSS length
    jz      .Lbss_done
.Lbss_loop:
    aghi    %r3, -1
    mvc     0(1,%r2), 0(%r2)            // extend zero one byte at a time
    // Actually use XC for efficiency — zero first byte then MVC-extend
    // Simpler: just store zeros with a loop since BSS is small (<4K)
    mvi     0(%r2), 0
    la      %r2, 1(%r2)
    brctg   %r3, .Lbss_loop
.Lbss_done:

    // ----------------------------------------------------------------
    // Step 3: Set up a 64-bit stack and call the C loader.
    //
    // The stack is in BSS (just zeroed). We need 160 bytes of save area
    // below the stack pointer per the z/Architecture ABI.
    // R2 = schid, loaded from lowcore 0xB8 (set by the channel subsystem
    // at IPL time to the subchannel ID of the IPL device).
    // ----------------------------------------------------------------
    larl    %r15, stage1_stack_top
    aghi    %r15, -160

    llgf    %r2, 0xb8                   // load schid from lowcore (31-bit zero-extend)

    brasl   %r14, zxfl00_entry

    // ----------------------------------------------------------------
    // Step 4: Halt — disabled wait PSW.
    //
    // lpswe loads a 64-bit PSW. Bit 6 (wait) = 1, bit 7 (problem) = 0,
    // bit 12 (64-bit) = 1. Address 0xDEAD0000 is the halt indicator.
    // ----------------------------------------------------------------
.Lhalt:
    larl    %r1, .Ldisabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt                      // unreachable; silences assembler warning

    .align 8
.Ldisabled_wait_psw:
    .quad   0x0000800180000000          // 64-bit disabled-wait PSW (bit16=wait, bits31+32=64bit)
    .quad   0x00000000DEAD0001          // halt address (operator-visible)

    .section .bss
    .align 16
stage1_stack_bottom:
    .skip   8192
stage1_stack_top:

    .section .note.GNU-stack, "", @progbits
