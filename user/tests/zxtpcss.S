// SPDX-License-Identifier: Apache-2.0
/// @file user/tests/zxtpcss.S — PC-ss suite callees

    .text

    .globl zxt_rpc
    .type zxt_rpc, @function
    .balign 2
zxt_rpc:
    // r0 = mix(r2, r3, r5 ^ r6)
    lgr     %r11, %r5
    xgr     %r11, %r6              // c = seq ^ K1
    larl    %r12, .Lmix_km
    lg      %r12, 0(%r12)
    msgr    %r11, %r12             // c * KM
    rllg    %r12, %r3, 17          // rotl64(salt, 17)
    lgr     %r0, %r2
    xgr     %r0, %r12
    xgr     %r0, %r11              // x = a ^ rot ^ c*KM
    srlg    %r12, %r0, 29
    xgr     %r0, %r12              // x ^= x >> 29
    larl    %r12, .Lmix_k2
    lg      %r12, 0(%r12)
    msgr    %r0, %r12              // x *= K2
    srlg    %r12, %r0, 32
    xgr     %r0, %r12              // x ^= x >> 32
    // r1 = mix(r0, 0xBEEF, r2) — K2 is a callee-side constant.
    llilf   %r6, 0xBEEF
    lgr     %r11, %r2
    larl    %r12, .Lmix_km
    lg      %r12, 0(%r12)
    msgr    %r11, %r12             // nonce * KM
    rllg    %r12, %r6, 17          // rotl64(K2, 17)
    lgr     %r1, %r0
    xgr     %r1, %r12
    xgr     %r1, %r11
    srlg    %r12, %r1, 29
    xgr     %r1, %r12
    larl    %r12, .Lmix_k2
    lg      %r12, 0(%r12)
    msgr    %r1, %r12
    srlg    %r12, %r1, 32
    xgr     %r1, %r12
    // Clobber the caller-visible arguments: PR must restore them from
    // the state entry, and the caller verifies every one.
    lghi    %r2, 0x5A5A
    llilf   %r3, 0xC3C3
    llilf   %r4, 0x1BAD            // overwrites the entry parameter
    llilf   %r5, 0xD00D
    llilf   %r6, 0xF00D
    pr

    .globl zxt_defer_entry
    .type zxt_defer_entry, @function
    .balign 2
zxt_defer_entry:
    svc     67                      // NB_GATE_DEFER; never returns
0:  j       0b

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
    oill    %r4, 1
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
    jh      .Lnest_ret0
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
    pr
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
    stg     %r2, 0(%r9)
    lgr     %r2, %r6
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
