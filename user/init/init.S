// SPDX-License-Identifier: Apache-2.0
// @file user/init/init.S
// @brief ZXFoundation user-space init binary
//
// Tests: demand-paged memory allocation via nc_mem_alloc (SVC 4),
//        page fault handling, and nc_mem_free (SVC 5).

    .text
    .align 8
    .globl _start
    .type _start, @function

_start:
    // Print startup message
    larl    %r2, msg_start
    lghi    %r3, msg_start_len
    svc     21                  // console_write(buf=r2, len=r3)

    // ── Test 1: nc_mem_alloc (demand-paged) ──────────────────────────
    // Allocate 4096 bytes (1 page) via SVC 4.
    // r2 = size. Returns VA in r2, or ~0 on error.
    lghi    %r2, 4096
    svc     4                   // nc_mem_alloc(size=4096)

    // Check for error (~0)
    lghi    %r3, -1
    cgr     %r2, %r3
    je      .Lalloc_fail

    // Save the VA for later free
    lgr     %r10, %r2           // r10 = allocated VA

    // Print alloc success
    larl    %r2, msg_alloc_ok
    lghi    %r3, msg_alloc_ok_len
    svc     21

    // ── Test 2: Touch the page (triggers demand-paging fault) ────────
    // Write a known pattern to the allocated page.
    // This WILL page fault since vm_map created an anonymous region
    // with no physical backing — handle_page_fault must resolve it.
    lghi    %r3, 0x42           // pattern byte = 0x42
    stc     %r3, 0(%r10)        // store byte at VA (triggers page fault)

    // Read it back and verify
    llgc    %r4, 0(%r10)        // load byte back
    cgr     %r4, %r3
    jne     .Lverify_fail

    // Write more — fill first 8 bytes with a 64-bit pattern
    llihh   %r3, 0xDEAD
    oilh    %r3, 0xBEEF
    oihl    %r3, 0xCAFE
    oill    %r3, 0xBABE
    stg     %r3, 0(%r10)        // store 0xDEADBEEFCAFEBABE

    // Read back and verify
    lg      %r4, 0(%r10)
    cgr     %r4, %r3
    jne     .Lverify_fail

    // Print verify success
    larl    %r2, msg_verify_ok
    lghi    %r3, msg_verify_ok_len
    svc     21

    // ── Test 3: nc_mem_protect (read-only then back to RW) ───────────
    // Set the page to read-only (prot=0x1 = read)
    lgr     %r2, %r10           // r2 = VA
    lghi    %r3, 4096           // r3 = size
    lghi    %r4, 1              // r4 = prot (0x1 = read-only)
    svc     8                   // nc_mem_protect(va, size, prot)

    // Check result (0 = success)
    cghi    %r2, 0
    jne     .Lprotect_fail

    // Verify the data is still readable after protect change
    lg      %r4, 0(%r10)        // should still be 0xDEADBEEFCAFEBABE
    llihh   %r3, 0xDEAD
    oilh    %r3, 0xBEEF
    oihl    %r3, 0xCAFE
    oill    %r3, 0xBABE
    cgr     %r4, %r3
    jne     .Lverify_fail

    // Set back to read-write (prot=0x3 = read+write)
    lgr     %r2, %r10           // r2 = VA
    lghi    %r3, 4096           // r3 = size
    lghi    %r4, 3              // r4 = prot (0x3 = read+write)
    svc     8                   // nc_mem_protect(va, size, prot)

    cghi    %r2, 0
    jne     .Lprotect_fail

    // Write again after re-enabling write — this proves PTE was updated
    lghi    %r3, 0x55
    stc     %r3, 0(%r10)        // store byte — must not fault

    // Print protect success
    larl    %r2, msg_protect_ok
    lghi    %r3, msg_protect_ok_len
    svc     21

    // ── Test 4: nc_mem_free (single page) ────────────────────────────
    lgr     %r2, %r10           // r2 = VA
    lghi    %r3, 4096           // r3 = size
    svc     5                   // nc_mem_free(va, size)

    // Check result (0 = success)
    cghi    %r2, 0
    jne     .Lfree_fail

    // Print free success
    larl    %r2, msg_free_ok
    lghi    %r3, msg_free_ok_len
    svc     21

    // ── Test 5: Multi-page alloc (16KB = 4 pages) ────────────────────
    lgfi    %r2, 16384          // size = 4 pages
    svc     4                   // nc_mem_alloc(16384)

    lghi    %r3, -1
    cgr     %r2, %r3
    je      .Lalloc_fail

    lgr     %r11, %r2           // r11 = multi-page VA base

    // Touch each page to trigger 4 separate page faults
    lghi    %r3, 0xAA
    stc     %r3, 0(%r11)        // page 0
    lgr     %r5, %r11
    aghi    %r5, 4096
    stc     %r3, 0(%r5)         // page 1
    aghi    %r5, 4096
    stc     %r3, 0(%r5)         // page 2
    aghi    %r5, 4096
    stc     %r3, 0(%r5)         // page 3

    // Verify each page
    llgc    %r4, 0(%r11)
    cgr     %r4, %r3
    jne     .Lverify_fail
    lgr     %r5, %r11
    aghi    %r5, 4096
    llgc    %r4, 0(%r5)
    cgr     %r4, %r3
    jne     .Lverify_fail
    aghi    %r5, 4096
    llgc    %r4, 0(%r5)
    cgr     %r4, %r3
    jne     .Lverify_fail
    aghi    %r5, 4096
    llgc    %r4, 0(%r5)
    cgr     %r4, %r3
    jne     .Lverify_fail

    // Free the multi-page allocation
    lgr     %r2, %r11
    lgfi    %r3, 16384
    svc     5                   // nc_mem_free

    cghi    %r2, 0
    jne     .Lfree_fail

    // Print multi-page success
    larl    %r2, msg_multi_ok
    lghi    %r3, msg_multi_ok_len
    svc     21

    // ── All tests passed ─────────────────────────────────────────────
    larl    %r2, msg_pass
    lghi    %r3, msg_pass_len
    svc     21

    // Idle loop
.Lidle:
    svc     18                  // time_yield
    j       .Lidle

    // ── Error paths ──────────────────────────────────────────────────
.Lalloc_fail:
    larl    %r2, msg_alloc_fail
    lghi    %r3, msg_alloc_fail_len
    svc     21
    j       .Lidle

.Lverify_fail:
    larl    %r2, msg_verify_fail
    lghi    %r3, msg_verify_fail_len
    svc     21
    j       .Lidle

.Lfree_fail:
    larl    %r2, msg_free_fail
    lghi    %r3, msg_free_fail_len
    svc     21
    j       .Lidle

.Lprotect_fail:
    larl    %r2, msg_protect_fail
    lghi    %r3, msg_protect_fail_len
    svc     21
    j       .Lidle

    // ── String constants ─────────────────────────────────────────────
    .align 2
msg_start:
    .ascii  "[init] ZXFoundation init domain started\n"
    .equ    msg_start_len, . - msg_start

    .align 2
msg_alloc_ok:
    .ascii  "[init] TEST: nc_mem_alloc(4096) => OK\n"
    .equ    msg_alloc_ok_len, . - msg_alloc_ok

    .align 2
msg_verify_ok:
    .ascii  "[init] TEST: demand-page write+read verify => OK\n"
    .equ    msg_verify_ok_len, . - msg_verify_ok

    .align 2
msg_free_ok:
    .ascii  "[init] TEST: nc_mem_free => OK\n"
    .equ    msg_free_ok_len, . - msg_free_ok

    .align 2
msg_protect_ok:
    .ascii  "[init] TEST: nc_mem_protect RO+RW cycle => OK\n"
    .equ    msg_protect_ok_len, . - msg_protect_ok

    .align 2
msg_multi_ok:
    .ascii  "[init] TEST: multi-page alloc (4 pages) => OK\n"
    .equ    msg_multi_ok_len, . - msg_multi_ok

    .align 2
msg_pass:
    .ascii  "[init] ALL TESTS PASSED\n"
    .equ    msg_pass_len, . - msg_pass

    .align 2
msg_alloc_fail:
    .ascii  "[init] TEST FAILED: nc_mem_alloc returned error\n"
    .equ    msg_alloc_fail_len, . - msg_alloc_fail

    .align 2
msg_verify_fail:
    .ascii  "[init] TEST FAILED: demand-page verify mismatch\n"
    .equ    msg_verify_fail_len, . - msg_verify_fail

    .align 2
msg_free_fail:
    .ascii  "[init] TEST FAILED: nc_mem_free returned error\n"
    .equ    msg_free_fail_len, . - msg_free_fail

    .align 2
msg_protect_fail:
    .ascii  "[init] TEST FAILED: nc_mem_protect returned error\n"
    .equ    msg_protect_fail_len, . - msg_protect_fail
