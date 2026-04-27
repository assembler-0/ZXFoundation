// SPDX-License-Identifier: Apache-2.0
// arch/s390x/boot/entry.S - ZXFoundation bootstrap for s390x (arch8 / z900+)
//
// Execution flow:
//   1. Firmware (s390-ccw-virtio IPL) loads the ELF and jumps to 0x10000,
//      which is _start (guaranteed first by the linker script).
//   2. We load a known-good 64-bit z/Architecture PSW to ensure we are
//      running in the correct mode regardless of firmware state.
//   3. Stack pointer and frame pointer are initialised.
//   4. .bss is zeroed (C mandates this; firmware does not guarantee it).
//   5. We call zxfoundation_global_initialize(), which never returns.
//   6. If it somehow does, we enter a disabled wait state.
//
// Register state on entry from firmware:
//   All general-purpose registers: undefined (do not rely on any value)
//   PSW: firmware-defined; we reload it immediately in step 2

#include <zxfoundation/zconfig.h>

// ---------------------------------------------------------------------------
// Constants - sourced from zconfig.h
// ---------------------------------------------------------------------------
// CONFIG_BOOT_STACK_SIZE and CONFIG_S390X_SAVE_AREA come from zconfig.h

// ---------------------------------------------------------------------------
// PSW constants - sourced from zconfig.h
// ---------------------------------------------------------------------------
// CONFIG_PSW_ARCH_BITS and CONFIG_PSW_DISABLED_WAIT come from zconfig.h
#define PSW_ARCH_BITS       CONFIG_PSW_ARCH_BITS
#define PSW_DISABLED_WAIT   CONFIG_PSW_DISABLED_WAIT

    .section .text._start, "ax", @progbits
    .globl _start
    .type  _start, @function

_start:
    // -----------------------------------------------------------------------
    // Step 1: Load a known-good 64-bit z/Architecture PSW.
    //
    // LPSWE loads a new PSW from a doubleword-aligned 16-byte value.
    // We use it to guarantee we are in 64-bit mode with a clean flag state
    // before doing anything else. The new PSW address points to the
    // instruction immediately after the LPSWE itself (_start_64bit).
    // -----------------------------------------------------------------------
    larl    %r1, initial_psw
    lpswe   0(%r1)

    // Execution resumes here after LPSWE, now guaranteed in 64-bit z/Arch.
_start_64bit:

    // -----------------------------------------------------------------------
    // Step 2: Set up the stack.
    //
    // %r15 is the stack pointer. We point it at stack_top and subtract the
    // 160-byte register save area that the s390x ABI requires the caller to
    // have allocated before making any call.
    //
    // %r11 is the frame pointer. Setting it to 0 terminates the call chain
    // (a back-chain of 0 means "no caller"), which is correct for the root
    // frame of a kernel.
    // -----------------------------------------------------------------------
    larl    %r15, stack_top
    aghi    %r15, -CONFIG_S390X_SAVE_AREA
    lghi    %r11, 0

    // -----------------------------------------------------------------------
    // Step 3: Zero .bss.
    //
    // C guarantees that zero-initialised globals start at zero. The firmware
    // does not zero memory for us, so we must do it before calling any C.
    //
    // We use a simple byte-clear loop. For a small kernel this is fine;
    // a future optimisation could use MVCL or vector stores.
    //
    // Registers used (caller-saved, safe to clobber before first C call):
    //   %r2 = current pointer (starts at __bss_start)
    //   %r3 = end pointer    (__bss_end)
    //   %r0 = zero byte
    // -----------------------------------------------------------------------
    larl    %r2, __bss_start
    larl    %r3, __bss_end
    lghi    %r0, 0

.Lbss_zero_loop:
    cgr     %r2, %r3                // if current >= end, done
    jnl     .Lbss_zero_done
    stc     %r0, 0(%r2)             // store zero byte
    aghi    %r2, 1
    j       .Lbss_zero_loop

.Lbss_zero_done:

    // -----------------------------------------------------------------------
    // Step 4: Call the kernel entry point.
    //
    // zxfoundation_global_initialize() is [[noreturn]]. If it ever returns
    // (it shouldn't), we fall through to the disabled wait below.
    // -----------------------------------------------------------------------
    brasl   %r14, zxfoundation_global_initialize

    // -----------------------------------------------------------------------
    // Step 5: Disabled wait - should never be reached.
    //
    // LPSWE with a disabled-wait PSW halts the CPU and displays the PSW
    // in the QEMU monitor / hardware operator panel, making it easy to
    // see that something went wrong.
    // -----------------------------------------------------------------------
.Lhalt:
    larl    %r1, disabled_wait_psw
    lpswe   0(%r1)
    j       .Lhalt                  // LPSWE should not return; loop just in case

// ---------------------------------------------------------------------------
// PSW tables - must be 8-byte aligned
// ---------------------------------------------------------------------------
    .align 8

initial_psw:
    // 64-bit z/Architecture PSW, supervisor state, DAT off, interrupts off.
    // Address field points to _start_64bit (filled by the linker).
    .quad   PSW_ARCH_BITS, _start_64bit

disabled_wait_psw:
    // Disabled wait: CPU halts and shows this PSW on the operator panel.
    // Address 0 makes it obvious this is an abnormal termination.
    .quad   PSW_DISABLED_WAIT, 0x0000000000000000

// ---------------------------------------------------------------------------
// Boot stack - in .bss so it is covered by the zero-init loop above
// ---------------------------------------------------------------------------
    .section .bss
    .align 16

stack_bottom:
    .skip   CONFIG_BOOT_STACK_SIZE
stack_top:
