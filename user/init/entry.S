// SPDX-License-Identifier: Apache-2.0
/// @file user/init/entry.S — freestanding entry + PC-ss test primitives

    .text

    .globl _start
    .type _start, @function
    .balign 2
_start:
    lghi    %r1, 0
    sar     %a0, %r1
    sar     %a1, %r1
    sar     %a2, %r1
    sar     %a3, %r1
    sar     %a4, %r1
    sar     %a5, %r1
    sar     %a6, %r1
    sar     %a7, %r1
    sar     %a8, %r1
    sar     %a9, %r1
    sar     %a10, %r1
    sar     %a11, %r1
    sar     %a12, %r1
    sar     %a13, %r1
    sar     %a14, %r1
    sar     %a15, %r1
    aghi    %r15, -512
    larl    %r1, nb_source_token
    stg     %r10, 0(%r1)
    brasl   %r14, zx_init_main
.Lhang:
    j       .Lhang

    .globl pcss_rpc
    .type pcss_rpc, @function
    .balign 2
pcss_rpc:
    larl    %r1, pcss_spill_r7
    stg     %r7, 0(%r1)
    larl    %r1, pcss_spill_r15
    stg     %r15, 0(%r1)
    lgr     %r7, %r2               // pcnum
    lgr     %r2, %r3               // nonce
    lgr     %r3, %r4               // salt
    // r5 (seq) and r6 (k1) already sit in their callee-ABI slots.
    pc      0(%r7)
    larl    %r7, pcss_back_r15
    stg     %r15, 0(%r7)
    larl    %r7, pcss_reply_r0
    stg     %r0, 0(%r7)
    larl    %r7, pcss_reply_r1
    stg     %r1, 0(%r7)
    larl    %r7, pcss_back_r2
    stmg    %r2, %r6, 0(%r7)
    larl    %r7, pcss_spill_r7
    lg      %r7, 0(%r7)            // the C caller's r7 (callee-saved)
    larl    %r1, pcss_spill_r15
    lg      %r15, 0(%r1)           // the C caller's real stack pointer
    br      %r14

    .section .bss
    .balign 8
    .globl pcss_spill_r7
pcss_spill_r7:  .quad 0
    .globl pcss_spill_r15
pcss_spill_r15: .quad 0
    .text

    .globl nest_storm
    .type nest_storm, @function
    .balign 2
nest_storm:
    aghi    %r15, -96
    stmg    %r6, %r13, 16(%r15)
    larl    %r9, nest_depth_total
    stg     %r2, 0(%r9)
    cghi    %r2, 0
    je      .Lnest_ret0
.Lnest_push:
    // r2 = this level's depth d; pattern P(d) = d*K1 ^ seed.
    larl    %r9, .Lnest_k1
    lg      %r10, 0(%r9)
    lgr     %r7, %r2
    msgr    %r7, %r10
    xgr     %r7, %r3
    lgr     %r8, %r7
    xgr     %r8, %r10
    larl    %r9, nest_mark
    sllg    %r10, %r2, 3
    stg     %r7, 0(%r10,%r9)
    larl    %r4, .Lnest_pop
    oill    %r4, 1                 // 64-bit return-mode bit
    larl    %r5, .Lnest_next
    bakr    %r4, %r5
.Lnest_next:
    aghi    %r2, -1
    cghi    %r2, 0
    je      .Lnest_pop
    j       .Lnest_push

.Lnest_pop:
    aghi    %r2, 1
    larl    %r9, nest_depth_total
    lg      %r10, 0(%r9)
    cgr     %r2, %r10
    jh      .Lnest_ret0            // past the last entry: pre-nest bottom
    lghi    %r7, 0
    lghi    %r8, 0
    eregg   %r7, %r8               // 64-bit extract (B90E); EREG is low-32 only
    larl    %r9, .Lnest_k1
    lg      %r10, 0(%r9)
    lgr     %r11, %r7
    lgr     %r12, %r8
    lgr     %r7, %r2
    msgr    %r7, %r10
    xgr     %r7, %r3
    lgr     %r8, %r7
    xgr     %r8, %r10
    cgr     %r11, %r7
    jne     .Lnest_fail_ereg
    cgr     %r12, %r8
    jne     .Lnest_fail_ereg
    lgr     %r6, %r2
    lgr     %r7, %r3
    msta    %r6
    lghi    %r13, 3
    esta    %r10, %r13
    cr      %r10, %r6
    jne     .Lnest_fail_msta
    cr      %r11, %r7
    jne     .Lnest_fail_msta
    larl    %r9, nest_mark
    sllg    %r10, %r2, 3
    lg      %r11, 0(%r10,%r9)
    larl    %r9, .Lnest_k1
    lg      %r10, 0(%r9)
    lgr     %r7, %r2
    msgr    %r7, %r10
    xgr     %r7, %r3
    cgr     %r11, %r7
    jne     .Lnest_fail_mark
    pr                              // pop this entry; parent continues
.Lnest_ret0:
    lghi    %r2, 0
    j       .Lnest_epilogue
.Lnest_fail_ereg:
    lghi    %r6, 1
    j       .Lnest_fail
.Lnest_fail_msta:
    lghi    %r6, 2
    j       .Lnest_fail
.Lnest_fail_mark:
    lghi    %r6, 3
.Lnest_fail:
    larl    %r9, nest_fail_level
    stg     %r2, 0(%r9)            // r2 still holds the level d
    lgr     %r2, %r6               // return the failure kind
    j       .Lnest_epilogue
.Lnest_epilogue:
    lmg     %r6, %r13, 16(%r15)
    aghi    %r15, 96
    br      %r14

    .section .rodata
    .balign 8
.Lnest_k1:
    .quad   0x9E3779B97F4A7C15
.Lmix_km:
    .quad   0x9E3779B97F4A7C15
.Lmix_k2:
    .quad   0xBF58476D1CE4E5B9

    .section .note.GNU-stack,"",@progbits
