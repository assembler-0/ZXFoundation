// SPDX-License-Identifier: Apache-2.0
// arch/s390x/boot/entry.S - ZXFoundation bootstrap
//
// This is the very first code that runs. QEMU's s390-ccw-virtio firmware
// loads the kernel ELF and jumps to the physical load address (0x10000),
// which must be _start. The .text._start section is placed first by the
// linker script to guarantee this.
//
// Register conventions on entry (from firmware):
//   %r11 - not guaranteed; we zero it for the frame pointer
//   %r15 - not guaranteed; we set it to stack_top
//   All other registers: undefined

    .section .text._start, "ax", @progbits
    .globl _start
    .type _start, @function

_start:
    larl    %r15, stack_top         // Set stack pointer to top of boot stack
    aghi    %r15, -160              // Reserve standard s390x register save area
    lghi    %r11, 0                 // Zero frame pointer

    brasl   %r14, zxfoundation_global_initialize

    // zxfoundation_global_initialize is [[noreturn]], but if it ever
    // returns, fall through to a disabled wait PSW so the machine halts
    // cleanly rather than executing garbage.
halt:
    larl    %r1, disabled_wait
    lpswe   0(%r1)

.align 8
disabled_wait:
    // Disabled wait PSW: bit 6 (wait) + bit 7 (problem state disabled) set,
    // address 0 - machine stops and shows this PSW in the QEMU monitor.
    .quad 0x0002000180000000, 0x0000000000000000

.section .bss
.align 16
stack_bottom:
    .skip 16384                     // 16 KiB boot stack
stack_top:
