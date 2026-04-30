// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/stage2/init31.S - ZXFoundation bootstrap for ESA/390
//
// Based on FLATBOOT's proven architecture switching pattern

#include <zxfoundation/zconfig.h>

    .machinemode zarch
    
    .section .text.__zx_start_kernel31, "ax"
    .globl __zx_start_kernel31

__zx_start_kernel31:
    /// Check if z/Architecture mode is available
    stfl    0                       # Store facility list at address 0
    tm      200, 0x01               # Test bit 0 of byte 200 (z/Arch available?)
    bno     .Lno_zarch              # Branch if not available

    /// Switch to z/Architecture mode (ignore errors - may already be in z/Arch)
    lhi     %r1, 0x0001             # r1 = 1 (z/Architecture mode)
    sigp    %r1, %r0, 0x12          # Signal processor: set architecture (z/Arch)
    
    /// Establish addressability using base register
    /// We need to calculate the address of our 64-bit entry point
    /// Use LA chain to build base register (avoids 12-bit displacement limit)
    lhi     %r13, 0                 # Clear r13
    la      %r13, 2048(%r13)        # r13 = 2048
    la      %r13, 2048(%r13)        # r13 = 4096
    la      %r13, 2048(%r13)        # r13 = 6144
    la      %r13, 2048(%r13)        # r13 = 8192 (0x2000)
    la      %r13, 2048(%r13)        # r13 = 10240 (0x2800)
    la      %r13, 2048(%r13)        # r13 = 12288 (0x3000)
    la      %r13, 2048(%r13)        # r13 = 14336 (0x3800)
    la      %r13, 2048(%r13)        # r13 = 16384 (0x4000)
    la      %r13, 2048(%r13)        # r13 = 18432 (0x4800)
    la      %r13, 2048(%r13)        # r13 = 20480 (0x5000)
    la      %r13, 2048(%r13)        # r13 = 22528 (0x5800)
    la      %r13, 2048(%r13)        # r13 = 24576 (0x6000)
    la      %r13, 2048(%r13)        # r13 = 26624 (0x6800)
    la      %r13, 2048(%r13)        # r13 = 28672 (0x7000)
    la      %r13, 2048(%r13)        # r13 = 30720 (0x7800)
    la      %r13, 2048(%r13)        # r13 = 32768 (0x8000)
    la      %r13, 2048(%r13)        # r13 = 34816 (0x8800)
    la      %r13, 2048(%r13)        # r13 = 36864 (0x9000)
    la      %r13, 2048(%r13)        # r13 = 38912 (0x9800)
    la      %r13, 2048(%r13)        # r13 = 40960 (0xA000)
    la      %r13, 2048(%r13)        # r13 = 43008 (0xA800)
    la      %r13, 2048(%r13)        # r13 = 45056 (0xB000)
    la      %r13, 2048(%r13)        # r13 = 47104 (0xB800)
    la      %r13, 2048(%r13)        # r13 = 49152 (0xC000)
    la      %r13, 2048(%r13)        # r13 = 51200 (0xC800)
    la      %r13, 2048(%r13)        # r13 = 53248 (0xD000)
    la      %r13, 2048(%r13)        # r13 = 55296 (0xD800)
    la      %r13, 2048(%r13)        # r13 = 57344 (0xE000)
    la      %r13, 2048(%r13)        # r13 = 59392 (0xE800)
    la      %r13, 2048(%r13)        # r13 = 61440 (0xF000)
    la      %r13, 2048(%r13)        # r13 = 63488 (0xF800)
    la      %r13, 2048(%r13)        # r13 = 65536 (0x10000) = kernel load address!

    /// Now r13 points to 0x10000 (our load address)
    /// Calculate offset to start2_64 and set AM64 bit
    la      %r8, (start2_64 - __zx_start_kernel31 + 1)(%r13)  # +1 sets AM64 bit
    
    /// Branch and Set Mode - switches to 64-bit addressing
    bsm     %r0, %r8                # r0 = unused, r8 = target address with AM64

start2_64:
    /// Now in 64-bit mode - can use z/Architecture instructions
    larl    %r15, stack_top
    aghi    %r15, -CONFIG_S390X_SAVE_AREA
    lghi    %r11, 0

    larl    %r2, __bss_start
    larl    %r3, __bss_end
    lghi    %r0, 0
.Lbss_zero_loop:
    cgr     %r2, %r3
    jnl     .Lbss_zero_done
    stc     %r0, 0(%r2)
    aghi    %r2, 1
    j       .Lbss_zero_loop
.Lbss_zero_done:

    brasl   %r14, trap_init
    brasl   %r14, zxfoundation_global_initialize

.Lhalt:
    larl    %r1, disabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt

.Lno_zarch:
    /// z/Architecture not available - enter disabled wait state
    lpsw    .Lold_wait_psw

    .balign 8
.Lold_wait_psw:
    .long   0x000a0000, 0xdeadbeef

disabled_wait_psw:
    .quad   CONFIG_PSW_DISABLED_WAIT, 0x0000000000000000

    .section .bss
    .balign 16

stack_bottom:
    .skip   CONFIG_BOOT_STACK_SIZE
stack_top:

    .section .note.GNU-stack, "", @progbits