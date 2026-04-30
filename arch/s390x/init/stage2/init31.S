// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/stage2/init31.S - ZXFoundation bootstrap for ESA/390

#include <zxfoundation/zconfig.h>

    .machinemode zarch
    .section .text.__zx_start_kernel31, "ax"
    .globl __zx_start_kernel31

__zx_start_kernel31:
    stfl    0                       
    tm      200, 0x01               
    bno     .Lno_zarch              

    lhi     %r1, 0x0001             
    sigp    %r1, %r0, 0x12          
    
    lhi     %r13, 0                 
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        
    la      %r13, 2048(%r13)        

    larl    %r8, __zx_start_kernel64  
    
    oilh    %r8, 0x8000
    bsm     %r0, %r8                

.Lno_zarch:
    lpsw    .Lold_wait_psw

    .balign 8
.Lold_wait_psw:
    .long   0x000a0000, 0xdeadbeef

    .section .note.GNU-stack, "", @progbits