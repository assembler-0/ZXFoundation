// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/zxfl_ipl.S - ZXFoundation Loader Entry

#include <zxfoundation/zconfig.h>

    .section .text.ipl, "ax"
    .globl __zx_ipl_entry
    .org 0x0

__zx_ipl_entry:
    // IPL PSW (8 bytes)
    .long   0x00080000              // 31-bit addressing mode
    .long   0x80000000 + zxfl_loader_entry  // Jump to start

    // IPL CCWs (16 bytes, unused by us directly, reserved for hw)
    .long   0x00000000, 0x00000000
    .long   0x00000000, 0x00000000

    .section .text, "ax"
    .globl zxfl_loader_entry
zxfl_loader_entry:
    // Setup a temporary stack for C code
    larl    %r15, zxfl_stack_top
    ahi     %r15, -CONFIG_S390X_SAVE_AREA
    
    brasl   %r14, zxfl_main

.Lhalt:
    larl    %r1, disabled_wait_psw
    lpsw    0(%r1)

    .align 8
disabled_wait_psw:
    .long   0x000A0000
    .long   0xDEADDEAD

    .section .bss
    .align 16
zxfl_stack_bottom:
    .skip   CONFIG_BOOT_STACK_SIZE
zxfl_stack_top:

    .section .text, "ax"
    .globl detect_memory_size
detect_memory_size:
    // Save non-volatile registers
    stm     %r6, %r15, 24(%r15)
    
    // R2 is the address counter (start at 2MB to avoid kernel area)
    lhi     %r2, 0
    al      %r2, .L2MB

    // Save old PGM new PSW (at 0x68-0x6F in lowcore)
    l       %r3, 0x68
    l       %r4, 0x6C
    
    // Set custom PGM new PSW to catch the exception
    // Format: bits 0-7 = flags (0x08 = 31-bit), bits 8-31 = address
    larl    %r5, .Lpgm_catch
    o       %r5, .Lmask31       // Set bit 0 for 31-bit addressing
    lhi     %r6, 8              // PSW flags: 0x00080000
    sll     %r6, 16
    st      %r6, 0x68           // Store PSW word 0
    st      %r5, 0x6C           // Store PSW word 1 (address)

.Lloop:
    // Test Protection - will throw addressing exception when memory doesn't exist
    tprot   0(%r2), 0
    
    // If we get here, memory exists - try next MB
    al      %r2, .L1MB
    
    // Safety limit: stop at 2GB to avoid infinite loop
    cl      %r2, .Lmax_mem
    jl      .Lloop

.Lpgm_catch:
    // Exception caught! R2 currently holds the invalid address (memory size)
    // Restore the old PGM new PSW
    st      %r3, 0x68
    st      %r4, 0x6C
    
    // Return size in R2
    lm      %r6, %r15, 24(%r15)
    br      %r14

    .align 4
.Lmask31:   .long 0x80000000
.L1MB:      .long 0x00100000
.L2MB:      .long 0x00200000
.Lmax_mem:  .long 0x80000000    // 2GB limit

    .section .note.GNU-stack, "", @progbits
