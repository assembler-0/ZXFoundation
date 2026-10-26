// SPDX-License-Identifier: Apache-2.0
/// @file user/tests/common.S — shared freestanding entry + worker trampoline

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
    brasl   %r14, zxt_main
.Lhang:
    j       .Lhang

    .globl zxt_wentry0
    .type zxt_wentry0, @function
    .balign 2
zxt_wentry0:
    larl    %r15, zxt_wstack0
    aghi    %r15, 3920
    larl    %r1, zxt_worker_table
    lg      %r2, 0(%r1)
    ltgr    %r2, %r2
    jz      .Lpark0
    basr    %r14, %r2
.Lpark0:
    svc     224
    j       .Lpark0

    .globl zxt_wentry1
    .type zxt_wentry1, @function
    .balign 2
zxt_wentry1:
    larl    %r15, zxt_wstack1
    aghi    %r15, 3920
    larl    %r1, zxt_worker_table
    lg      %r2, 8(%r1)
    ltgr    %r2, %r2
    jz      .Lpark1
    basr    %r14, %r2
.Lpark1:
    svc     224
    j       .Lpark1

    .globl zxt_wentry2
    .type zxt_wentry2, @function
    .balign 2
zxt_wentry2:
    larl    %r15, zxt_wstack2
    aghi    %r15, 3920
    larl    %r1, zxt_worker_table
    lg      %r2, 16(%r1)
    ltgr    %r2, %r2
    jz      .Lpark2
    basr    %r14, %r2
.Lpark2:
    svc     224
    j       .Lpark2

    .globl zxt_wentry3
    .type zxt_wentry3, @function
    .balign 2
zxt_wentry3:
    larl    %r15, zxt_wstack3
    aghi    %r15, 3920
    larl    %r1, zxt_worker_table
    lg      %r2, 24(%r1)
    ltgr    %r2, %r2
    jz      .Lpark3
    basr    %r14, %r2
.Lpark3:
    svc     224
    j       .Lpark3

    .section .note.GNU-stack,"",@progbits

    .bss
    .balign 8
    .globl zxt_wstack0
zxt_wstack0:
    .skip 4096
    .balign 8
    .globl zxt_wstack1
zxt_wstack1:
    .skip 4096
    .balign 8
    .globl zxt_wstack2
zxt_wstack2:
    .skip 4096
    .balign 8
    .globl zxt_wstack3
zxt_wstack3:
    .skip 4096
