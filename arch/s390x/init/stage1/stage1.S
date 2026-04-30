/// SPDX-License-Identifier: Apache-2.0
/// arch/s390x/init/stage1/stage1.S - ZXFoundation IPL Stage 1 Bootloader
///
/// @brief ESA/390 31-bit IPL loader that dynamically locates and loads the
///        kernel from DASD track 15 using CCW I/O operations.
///
/// Boot sequence:
///   1. Hardware loads IPL1 (24 bytes) from cyl 0, trk 0, rec 1 → address 0x0
///   2. Hardware executes LPSW 0x0 → jumps to loader_entry (offset 0x18)
///   3. Loader issues SSCH with CCW chain to read kernel from track 15
///   4. Loader polls TSCH until I/O completes
///   5. Loader executes LPSW to jump to kernel at 0x10400
///
/// CRITICAL: ESA/390 31-bit mode only (no z/Architecture instructions)
/// CRITICAL: IPL1 section must be exactly 24 bytes

    .section .text.ipl1, "ax"
    .globl __zx_ipl1_record
    .org 0x0

__zx_ipl1_record:
    ///
    /// IPL PSW (8 bytes) - ESA/390 format
    /// Points to loader_entry at offset 0x18
    ///
    .long   0x00080000              # PSW flags: I/O enabled, 31-bit
    .long   0x80000000 + loader_entry  # PSW address: 31-bit mode, loader_entry

    ///
    /// Padding to 24 bytes (IPL1 record size)
    ///
    .long   0x00000000, 0x00000000
    .long   0x00000000, 0x00000000

    ///
    /// Loader code starts at offset 0x18
    ///
    .section .text.loader, "ax"
    .globl loader_entry
loader_entry:
    /// Establish base register
    basr    %r13, 0                 # r13 = current address
.Lbase:

    /// Get subchannel ID from lowcore 0xB8
    l       %r1, 0xb8               # r1 = subchannel ID

    /// Issue Start Subchannel (SSCH) to load kernel
    la      %r2, orb - .Lbase(%r13) # r2 = address of ORB
    ssch    0(%r2)                  # Start I/O operation

.Lwait_io:
    /// Poll with Test Subchannel (TSCH) until I/O completes
    la      %r3, irb - .Lbase(%r13) # r3 = address of IRB (I/O Response Block)
    tsch    0(%r3)                  # Test subchannel status
    bc      4, .Lwait_io            # Branch if CC=1 (status pending)

    /// I/O complete - jump to kernel entry point
    la      %r1, kernel_psw - .Lbase(%r13)  # r1 = address of kernel PSW
    lpsw    0(%r1)                  # Load PSW and jump to kernel

    ///
    /// Data structures (relocated to avoid lowcore corruption)
    ///
    .org 0x200
    .align 8

    /// Operation Request Block (ORB) - 32 bytes
orb:
    .long   0x00000000              # Interrupt parameter
    .long   0x0000FF00              # Key=0, Format=0, LPM=0xFF
    .long   ccw_chain               # CCW address (absolute)
    .long   0x00000000              # Reserved
    .long   0x00000000, 0x00000000  # Reserved
    .long   0x00000000, 0x00000000  # Reserved

    /// CCW Chain - Read kernel from track 15
    /// CCW Format 0: cmd(1) | addr(3) | flags(1) | rsvd(1) | count(2)
    .align 8
ccw_chain:
    /// SEEK to cylinder 0, head 15 (track 15)
    .long   0x07000000 + seek_arg   # Command 0x07, address of seek_arg
    .long   0x60000006              # Flags: CC+SLI, Count: 6 bytes

    /// SEARCH for record 1
    .long   0x31000000 + search_arg # Command 0x31, address of search_arg
    .long   0x60000005              # Flags: CC+SLI, Count: 5 bytes

    /// TIC back to SEARCH if not found
    .long   0x08000000 + (ccw_chain + 8)  # Command 0x08 TIC, target address
    .long   0x00000000              # Flags: none, Count: 0

    /// READ kernel records (13 x 4KB blocks = 52KB)
    .long   0x06010000, 0x60001000  # READ to 0x10000, CC+SLI, 4KB
    .long   0x06011000, 0x60001000  # READ to 0x11000, CC+SLI, 4KB
    .long   0x06012000, 0x60001000  # READ to 0x12000, CC+SLI, 4KB
    .long   0x06013000, 0x60001000  # READ to 0x13000, CC+SLI, 4KB
    .long   0x06014000, 0x60001000  # READ to 0x14000, CC+SLI, 4KB
    .long   0x06015000, 0x60001000  # READ to 0x15000, CC+SLI, 4KB
    .long   0x06016000, 0x60001000  # READ to 0x16000, CC+SLI, 4KB
    .long   0x06017000, 0x60001000  # READ to 0x17000, CC+SLI, 4KB
    .long   0x06018000, 0x60001000  # READ to 0x18000, CC+SLI, 4KB
    .long   0x06019000, 0x60001000  # READ to 0x19000, CC+SLI, 4KB
    .long   0x0601A000, 0x60001000  # READ to 0x1A000, CC+SLI, 4KB
    .long   0x0601B000, 0x60001000  # READ to 0x1B000, CC+SLI, 4KB
    .long   0x0601C000, 0x20001000  # READ to 0x1C000, SLI (last), 4KB

    /// SEEK argument: BBCCHH format (Bin, Cylinder, Head)
    .align 2
seek_arg:
    .byte   0x00, 0x00              # Bin 0, Cylinder 0 (high byte)
    .byte   0x00, 0x01              # Cylinder 1 (low byte), Head 0
    .byte   0x00, 0x00              # Padding

    /// SEARCH argument: CCHHR format (Cylinder, Head, Record)
    .align 2
search_arg:
    .byte   0x00, 0x01              # Cylinder 1
    .byte   0x00                    # Head 0 (track 15 = cyl 1, head 0)
    .byte   0x00                    # Record 0
    .byte   0x01                    # Record 1

    /// Kernel entry PSW
    .align 8
kernel_psw:
    .long   0x00080000              # PSW flags: I/O enabled, 31-bit
    .long   0x80010000              # PSW address: kernel entry at 0x10000

    /// I/O Response Block (IRB) - 96 bytes
    .align 8
irb:
    .skip   96

    .section .note.GNU-stack, "", @progbits
