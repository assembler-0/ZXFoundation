/// SPDX-License-Identifier: Apache-2.0
/// arch/s390x/init/stage1/stage1.S - ZXFoundation IPL Stage 1 Bootloader

    .section .text.ipl1, "ax"
    .globl __zx_ipl1_record
    .org 0x0

__zx_ipl1_record:
    .long   0x00080000              
    .long   0x80000000 + loader_entry  

    .long   0x00000000, 0x00000000
    .long   0x00000000, 0x00000000

    .section .text.loader, "ax"
    .globl loader_entry
loader_entry:
    basr    %r13, 0                 
.Lbase:
    l       %r1, 0xb8               

    la      %r2, orb - .Lbase(%r13) 
    ssch    0(%r2)                  

.Lwait_io:
    la      %r3, irb - .Lbase(%r13) 
    tsch    0(%r3)                  
    bc      4, .Lwait_io            

    la      %r1, kernel_psw - .Lbase(%r13)  
    lpsw    0(%r1)                  

    .org 0x200
    .align 8

    /// Operation Request Block (ORB) - 32 bytes
orb:
    .long   0x00000000              
    .long   0x0000FF00              
    .long   ccw_chain               
    .long   0x00000000              
    .long   0x00000000, 0x00000000  
    .long   0x00000000, 0x00000000  

    /// CCW Chain - Read kernel from track 15
    /// CCW Format 0: cmd(1) | addr(3) | flags(1) | rsvd(1) | count(2)
    .align 8
ccw_chain:
    /// SEEK to cylinder 0, head 15 (track 15)
    .long   0x07000000 + seek_arg   
    .long   0x60000006              

    /// SEARCH for record 1
    .long   0x31000000 + search_arg 
    .long   0x60000005              

    /// TIC back to SEARCH if not found
    .long   0x08000000 + (ccw_chain + 8)  
    .long   0x00000000              

    /// READ kernel records (13 x 4KB blocks = 52KB)
    .long   0x06010000, 0x60001000  
    .long   0x06011000, 0x60001000  
    .long   0x06012000, 0x60001000  
    .long   0x06013000, 0x60001000  
    .long   0x06014000, 0x60001000  
    .long   0x06015000, 0x60001000  
    .long   0x06016000, 0x60001000  
    .long   0x06017000, 0x60001000  
    .long   0x06018000, 0x60001000  
    .long   0x06019000, 0x60001000  
    .long   0x0601A000, 0x60001000  
    .long   0x0601B000, 0x60001000  
    .long   0x0601C000, 0x20001000  

    /// SEEK argument: BBCCHH format (Bin, Cylinder, Head)
    .align 2
seek_arg:
    .byte   0x00, 0x00              
    .byte   0x00, 0x01              
    .byte   0x00, 0x00              

    /// SEARCH argument: CCHHR format (Cylinder, Head, Record)
    .align 2
search_arg:
    .byte   0x00, 0x01              
    .byte   0x00                    
    .byte   0x00                    
    .byte   0x01                    

    .align 8
kernel_psw:
    .long   0x00080000              
    .long   0x80010000              

    /// I/O Response Block (IRB) - 96 bytes
    .align 8
irb:
    .skip   96

    .section .note.GNU-stack, "", @progbits
