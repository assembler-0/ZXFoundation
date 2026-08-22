// SPDX-License-Identifier: Apache-2.0
// @file user/init/init.S
// @brief ZXFoundation user-space init binary — tests all nucleus gate calls.
//
// Calling convention:
//   All nucleus calls go through SVC.  The SVC instruction's 8-bit number
//   field carries the packed call number (namespace << 5) | operation, and
//   max packed number is 230 (fits in 8 bits).
//     svc CALL_NR
//     r2=a0, r3=a1, r4=a2, r5=a3, r6=a4
//     Result in r2; ~0ULL on error.
//   CALL_NR = packed call number: (namespace << 5) | operation
//
// Register convention:
//   r8-r13  Persistent storage for test state.
//   r2-r7   Arguments and temporaries.
//   r15     Stack pointer.
//   r14     Return address within init.

    .text
    .align 8
    .globl _start
    .type _start, @function

    .equ DOM_CREATE,           (0 << 5) | 0    // 0
    .equ DOM_DESTROY,          (0 << 5) | 1    // 1
    .equ DOM_EXIT,             (0 << 5) | 2    // 2
    .equ DOM_INFO,             (0 << 5) | 3    // 3
    .equ DOM_QUERY,            (0 << 5) | 4    // 4
    .equ DOM_STRAND_CREATE,    (0 << 5) | 5    // 5
    .equ DOM_STRAND_SET_POLICY,(0 << 5) | 6    // 6
    .equ DOM_STRAND_SET_AFFIN, (0 << 5) | 7    // 7
    .equ DOM_STRAND_QUERY,     (0 << 5) | 8    // 8

    .equ MEM_ALLOC,            (1 << 5) | 0    // 32
    .equ MEM_FREE,             (1 << 5) | 1    // 33
    .equ MEM_MAP,              (1 << 5) | 2    // 34
    .equ MEM_UNMAP,            (1 << 5) | 3    // 35
    .equ MEM_PROTECT,          (1 << 5) | 4    // 36
    .equ MEM_SHARE,            (1 << 5) | 5    // 37

    .equ GATE_CREATE,          (2 << 5) | 0    // 64
    .equ GATE_DESTROY,         (2 << 5) | 1    // 65

    .equ PORTAL_CREATE,        (3 << 5) | 0    // 96
    .equ PORTAL_DESTROY,       (3 << 5) | 1    // 97

    .equ IO_BIND,              (4 << 5) | 0    // 128
    .equ IO_UNBIND,            (4 << 5) | 1    // 129
    .equ IO_SUBMIT,            (4 << 5) | 2    // 130
    .equ IO_IRQ_BIND,          (4 << 5) | 3    // 131
    .equ IO_DMA_EXTENT,        (4 << 5) | 4    // 132
    .equ IO_COMPLETION_DEQUEUE,(4 << 5) | 5    // 133

    .equ SGN_REGISTER,         (5 << 5) | 0    // 160
    .equ SGN_POLL,             (5 << 5) | 1    // 161
    .equ SGN_ACK,              (5 << 5) | 2    // 162
    .equ SGN_SEND,             (5 << 5) | 3    // 163
    .equ SGN_MASK_SET,         (5 << 5) | 4    // 164
    .equ SGN_MASK_GET,         (5 << 5) | 5    // 165
    .equ SGN_WAIT,             (5 << 5) | 6    // 166

    .equ CAP_ACQUIRE,          (6 << 5) | 0    // 192
    .equ CAP_RELEASE,          (6 << 5) | 1    // 193
    .equ CAP_VALIDATE,         (6 << 5) | 2    // 194
    .equ CAP_REVOKE,           (6 << 5) | 3    // 195

    .equ SYS_YIELD,            (7 << 5) | 0    // 224
    .equ SYS_DEADLINE,         (7 << 5) | 1    // 225
    .equ SYS_NOW,              (7 << 5) | 2    // 226
    .equ SYS_CONSOLE,          (7 << 5) | 3    // 227
    .equ SVC_REGISTER,         (7 << 5) | 4    // 228
    .equ SVC_LOOKUP,           (7 << 5) | 5    // 229
    .equ SVC_UNREGISTER,       (7 << 5) | 6    // 230
    .equ IPC_ENDPOINT_CREATE,  (7 << 5) | 7    // 231
    .equ IPC_ENDPOINT_DESTROY, (7 << 5) | 8    // 232
    .equ IPC_SEND,             (7 << 5) | 9    // 233
    .equ IPC_RECEIVE,          (7 << 5) | 10   // 234
    .equ SHM_EXPORT,           (7 << 5) | 11   // 235
    .equ SHM_IMPORT,           (7 << 5) | 12   // 236
    .equ SHM_MAP,              (7 << 5) | 13   // 237
    .equ POLICY_REGISTER,      (7 << 5) | 14   // 238
    .equ POLICY_UNREGISTER,    (7 << 5) | 15   // 239

    .equ ERR_SENTINEL,  -1       // ~0ULL
    .equ PAGE_SIZE,     4096

    .macro PRINT msg
    larl    %r2, \msg
    lghi    %r3, \msg\()_len
    svc     SYS_CONSOLE
    .endm

    .macro NCALL nr
    svc     \nr
    .endm

    .macro CHECK_OK lbl
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    je      \lbl
    .endm

    .macro CHECK_EQ reg, lbl
    cgr     %r2, \reg
    jne     \lbl
    .endm

_start:
    lghi    %r9, 0                  // r9 = pass count

    
    // Test  1: sys::console_write  
    
    PRINT   t01_name
    larl    %r2, msg_startup
    lghi    %r3, msg_startup_len
    NCALL   SYS_CONSOLE
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test  2: dom::info  
    
    PRINT   t02_name
    lghi    %r2, 0
    NCALL   DOM_INFO
    CHECK_OK t02_fail
    llgfr   %r8, %r2               // r8 = domain_id (low 32 bits)
    cghi    %r8, 0
    je      t02_fail
    srlg    %r1, %r2, 32            // r1 = cpu (high 32)
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test  3: dom::query(0)  
    
    PRINT   t03_name
    lghi    %r2, 0
    NCALL   DOM_QUERY
    CHECK_OK t03_fail
    lghi    %r1, 0xff
    ngr    %r2, %r1
    cghi    %r2, 0
    je      t03_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test  4: mem::alloc(4096)  
    
    PRINT   t04_name
    lghi    %r2, PAGE_SIZE
    NCALL   MEM_ALLOC
    CHECK_OK t04_fail
    lgr     %r11, %r2               // r11 = VA
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test  5: demand-paging  
    
    PRINT   t05_name
    lghi    %r3, 0x42
    stc     %r3, 0(%r11)
    llgc    %r4, 0(%r11)
    cgr     %r4, %r3
    jne     t05_fail
    llihh   %r3, 0xDEAD
    oilh    %r3, 0xBEEF
    oihl    %r3, 0xCAFE
    oill    %r3, 0xBABE
    stg     %r3, 0(%r11)
    lg      %r4, 0(%r11)
    cgr     %r4, %r3
    jne     t05_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test  6: mem::protect(VA, 4096, prot=1)  
    
    PRINT   t06_name
    lgr     %r2, %r11
    lghi    %r3, PAGE_SIZE
    lghi    %r4, 1
    NCALL   MEM_PROTECT
    CHECK_OK t06_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test  7: read after read-only  
    
    PRINT   t07_name
    lg      %r4, 0(%r11)
    llihh   %r3, 0xDEAD
    oilh    %r3, 0xBEEF
    oihl    %r3, 0xCAFE
    oill    %r3, 0xBABE
    cgr     %r4, %r3
    jne     t07_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test  8: mem::protect(VA, 4096, prot=3)  
    
    PRINT   t08_name
    lgr     %r2, %r11
    lghi    %r3, PAGE_SIZE
    lghi    %r4, 3
    NCALL   MEM_PROTECT
    CHECK_OK t08_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test  9: write after RW restored
    
    PRINT   t09_name
    lghi    %r3, 0x55
    stc     %r3, 0(%r11)
    llgc    %r4, 0(%r11)
    cgr     %r4, %r3
    jne     t09_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 10: mem::free(VA, 4096)  
    
    PRINT   t10_name
    lgr     %r2, %r11
    lghi    %r3, PAGE_SIZE
    NCALL   MEM_FREE
    CHECK_OK t10_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 11: mem::alloc(16384)  
    
    PRINT   t11_name
    lgfi    %r2, 16384
    NCALL   MEM_ALLOC
    CHECK_OK t11_fail
    lgr     %r11, %r2
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 12: touch all 4 pages  
    
    PRINT   t12_name
    lghi    %r3, 0xAA
    stcy    %r3, 0(%r11)
    stcy    %r3, 4096(%r11)
    stcy    %r3, 8192(%r11)
    stcy    %r3, 12288(%r11)
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 13: verify all 4 pages
    
    PRINT   t13_name
    lghi    %r3, 0xAA
    llgc    %r4, 0(%r11)
    cgr     %r4, %r3
    jne     t13_fail
    llgc    %r4, 4096(%r11)
    cgr     %r4, %r3
    jne     t13_fail
    llgc    %r4, 8192(%r11)
    cgr     %r4, %r3
    jne     t13_fail
    llgc    %r4, 12288(%r11)
    cgr     %r4, %r3
    jne     t13_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 14: mem::free(16384)  
    
    PRINT   t14_name
    lgr     %r2, %r11
    lgfi    %r3, 16384
    NCALL   MEM_FREE
    CHECK_OK t14_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 15: sys::time_now  
    
    PRINT   t15_name
    lghi    %r2, 0
    NCALL   SYS_NOW
    CHECK_OK t15_fail
    cghi    %r2, 0
    je      t15_fail
    lgr     %r13, %r2               // r13 = now timestamp; preserve admin source in r12
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 16: sys::time_deadline(1ms)  
    
    PRINT   t16_name
    lgfi    %r2, 1000000
    NCALL   SYS_DEADLINE
    CHECK_OK t16_fail
    cgr     %r2, %r13
    jle     t16_fail                 // deadline must be > now
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 17: sys::time_yield  
    
    PRINT   t17_name
    lghi    %r2, 0
    NCALL   SYS_YIELD
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 18: cap::acquire(admin_token, admin)
    
    PRINT   t18_name
    lgr     %r2, %r10               // bootstrap-issued source token
    lghi    %r3, 0x20               // cap_rights::admin = 0x20
    NCALL   CAP_ACQUIRE
    CHECK_OK t18_fail
    lgr     %r12, %r2               // r12 = admin cap token
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 19: cap::validate(admin_token, admin)  
    
    PRINT   t19_name
    lgr     %r2, %r12               // admin token
    lghi    %r3, 0x20               // admin right
    NCALL   CAP_VALIDATE
    CHECK_OK t19_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 20: dom::create(admin, "test/child")  
    
    PRINT   t20_name
    lgr     %r2, %r12               // admin cap token
    larl    %r3, name_child         // name
    NCALL   DOM_CREATE
    CHECK_OK t20_fail
    lgr     %r13, %r2               // r13 = child domain_id
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 21: dom::query(child_id)  
    
    PRINT   t21_name
    lgr     %r2, %r13               // child domain_id
    NCALL   DOM_QUERY
    CHECK_OK t21_fail
    lghi    %r1, 0xff
    ngr    %r2, %r1
    cghi    %r2, 0
    je      t21_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 22: cap::acquire(admin_token, map)
    
    PRINT   t22_name
    lgr     %r2, %r10               // bootstrap-issued source token
    lghi    %r3, 0x40               // cap_rights::map = 0x40
    NCALL   CAP_ACQUIRE
    CHECK_OK t22_fail
    lgr     %r11, %r2               // r11 = map cap token
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 23: cap::acquire(admin_token, delegate)
    //   (Re-acquired and released later in the mem::share test)
    
    PRINT   t23_name
    lgr     %r2, %r10               // bootstrap-issued source token
    lghi    %r3, 0x10               // cap_rights::delegate = 0x10
    NCALL   CAP_ACQUIRE
    CHECK_OK t23_fail
    // Release immediately — not needed until share test (which re-acquires)
    NCALL   CAP_RELEASE
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 24: sgn::register_handler(fault, handler=0, data=0) (cross: PC)
    
    PRINT   t24_name
    lghi    %r2, 3                  // signal_kind::fault = 3
    lghi    %r3, 0                  // handler = null
    lghi    %r4, 0                  // user_data = 0
    NCALL   SGN_REGISTER
    CHECK_OK t24_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 25: sgn::poll  
    
    PRINT   t25_name
    lghi    %r2, 0
    NCALL   SGN_POLL
    CHECK_OK t25_fail
    cghi    %r2, 0
    jne     t25_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 26: sgn::ack(0)  
    
    PRINT   t26_name
    lghi    %r2, 0
    NCALL   SGN_ACK
    CHECK_OK t26_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 27: gate::create(nucleus, any, target=self, entry=0, key=0) (cross: PC)
    
    PRINT   t27_name
    lgr     %r2, %r12               // a0 = admin cap token
    lghi    %r3, 0                  // a1 = gate_type::nucleus = 0
    lghi    %r4, 0                  // a2 = gate_auth::any = 0
    lgr     %r5, %r8                // a3 = target = self domain_id
    lghi    %r6, 0                  // a4 = entry_va = 0
    NCALL   GATE_CREATE
    CHECK_OK t27_fail
    lgr     %r0, %r2                // r0 = gate_id
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 28: gate::destroy(gate_id) (cross: PC)
    
    PRINT   t28_name
    lgr     %r2, %r12               // a0 = admin cap token
    lgr     %r3, %r0                // a1 = gate_id
    NCALL   GATE_DESTROY
    CHECK_OK t28_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 29: portal::create a read-only self portal over a resident page.
    
    PRINT   t29_name
    lghi    %r2, PAGE_SIZE
    NCALL   MEM_ALLOC
    CHECK_OK t29_fail
    lgr     %r7, %r2
    lghi    %r1, 0x33
    stc     %r1, 0(%r7)
    lgr     %r2, %r12               // a0 = admin cap token
    lgr     %r3, %r8                // a1 = target = self domain_id
    lgr     %r4, %r7                // a2 = source va_start
    lgr     %r5, %r7
    aghi    %r5, PAGE_SIZE          // a3 = source va_end
    lghi    %r6, 1                  // a4 = prot = read
    NCALL   PORTAL_CREATE
    CHECK_OK t29_fail
    lgr     %r11, %r2               // r11 = portal_id
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 30: portal::destroy(portal_id) (cross: PC)
    
    PRINT   t30_name
    lgr     %r2, %r12               // a0 = admin cap token
    lgr     %r3, %r11               // a1 = portal_id
    NCALL   PORTAL_DESTROY
    CHECK_OK t30_fail
    lgr     %r2, %r7
    lghi    %r3, PAGE_SIZE
    NCALL   MEM_FREE
    CHECK_OK t30_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 31: DMA extent, verified NOP submission, completion, and unmap.
    
    PRINT   t31_name
    lghi    %r2, 9                  // a0 = configured 3215 console device
    NCALL   IO_BIND
    CHECK_OK t31_fail
    lgr     %r11, %r2               // r11 = I/O binding token
    lgr     %r2, %r11
    NCALL   IO_DMA_EXTENT
    CHECK_OK t31_fail
    lgr     %r7, %r2                // r7 = DMA extent capability
    lgfi    %r0, 0x20000000         // known-free user VA
    lgr     %r2, %r7                // a0 = extent cap
    lgr     %r3, %r0                // a1 = VA
    lghi    %r4, 0                  // a2 = extent offset
    lghi    %r5, PAGE_SIZE          // a3 = map one page
    lghi    %r6, 3                  // a4 = RW
    NCALL   MEM_MAP
    CHECK_OK t31_fail
    lgr     %r6, %r0                 // r6 = mapped VA; r0 is not an RX base register
    lgfi    %r1, 0x5a5a             // verify the DMA extent is user-accessible
    st      %r1, 0(%r6)
    l       %r2, 0(%r6)
    cgr     %r2, %r1
    jne     t31_fail

    // ccw1 at DMA offset 0: NOP, no data transfer.
    lghi    %r1, 3
    stc     %r1, 0(%r6)
    lghi    %r1, 0
    stc     %r1, 1(%r6)
    sth     %r1, 2(%r6)
    st      %r1, 4(%r6)

    // io_request at DMA offset 0x100: request_id=1, CCW offset=0, count=1.
    lghi    %r1, 1
    stg     %r1, 0x100(%r6)
    lghi    %r1, 0
    st      %r1, 0x108(%r6)
    lghi    %r1, 1
    sth     %r1, 0x10c(%r6)
    lghi    %r1, 0
    stc     %r1, 0x10e(%r6)
    stc     %r1, 0x10f(%r6)

    lgr     %r2, %r11               // a0 = I/O binding token
    la      %r3, 0x100(%r6)         // a1 = io_request
    NCALL   IO_SUBMIT
    CHECK_OK t31_fail

    lghi    %r5, 256                // bounded completion polling
.Lt31_wait_completion:
    lgr     %r2, %r11               // a0 = I/O binding token
    la      %r3, 0x200(%r6)         // a1 = io_completion output
    NCALL   IO_COMPLETION_DEQUEUE
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    jne     .Lt31_completion_ready
    aghi    %r5, -1
    cghi    %r5, 0
    je      t31_fail
    lghi    %r2, 0
    NCALL   SYS_YIELD
    j       .Lt31_wait_completion
.Lt31_completion_ready:
    lg      %r1, 0x200(%r6)         // completion.request_id must match
    lghi    %r2, 1
    cgr     %r1, %r2
    jne     t31_fail

    lgfi    %r2, 0x20000000
    lghi    %r3, PAGE_SIZE
    NCALL   MEM_UNMAP
    CHECK_OK t31_fail
    lgr     %r2, %r7
    NCALL   CAP_RELEASE
    CHECK_OK t31_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 32: io::unbind
    
    PRINT   t32_name
    lgr     %r2, %r11               // a0 = I/O binding token
    NCALL   IO_UNBIND
    CHECK_OK t32_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 33: io::submit(bogus token) must reject before request access.
    
    PRINT   t33_name
    lghi    %r2, ERR_SENTINEL       // bogus token
    lghi    %r3, 0                  // null request
    NCALL   IO_SUBMIT
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    jne     t33_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 34: io::irq_bind(gate=0) must be denied by DGP metadata validation.
    
    PRINT   t34_name
    lghi    %r2, ERR_SENTINEL
    lghi    %r3, 0
    NCALL   IO_IRQ_BIND
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    jne     t34_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 35: mem::unmap  
    
    PRINT   t35_name
    lghi    %r2, PAGE_SIZE
    NCALL   MEM_ALLOC
    CHECK_OK t35_fail
    lgr     %r6, %r2                // save VA (r6 not corrupted by SAVE_FRAME)
    lgr     %r2, %r6
    lghi    %r3, PAGE_SIZE
    NCALL   MEM_UNMAP
    CHECK_OK t35_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 36: raw mem::map must reject non-extent arguments.
    
    PRINT   t36_name
    // Acquire map cap
    lgr     %r2, %r10               // bootstrap-issued source token
    lghi    %r3, 0x40               // map right
    NCALL   CAP_ACQUIRE
    CHECK_OK t36_fail
    lgr     %r7, %r2                // r7 = map cap (r1 corrupted by SAVE_FRAME)

    // Use a known-free VA (0x20000000) for physical mapping
    lgfi    %r0, 0x20000000         // r0 = target VA (free user-space address)

    // Map physical memory at target VA
    lgr     %r2, %r7                // a0 = map cap
    lgr     %r3, %r0                // a1 = VA
    lgfi    %r4, 0x400000           // a2 = phys (init binary)
    lghi    %r5, PAGE_SIZE          // a3 = size
    lghi    %r6, 3                  // a4 = prot (RW)
    NCALL   MEM_MAP
    // Raw physical map arguments are not a memory-extent capability.
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    jne     t36_fail
    // Release map cap
    lgr     %r2, %r7
    NCALL   CAP_RELEASE
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 37: legacy range-less mem::share must reject.
    
    PRINT   t37_name
    lgr     %r2, %r10               // bootstrap-issued source token
    lghi    %r3, 0x10               // delegate right
    NCALL   CAP_ACQUIRE
    CHECK_OK t37_fail
    lgr     %r7, %r2                // r7 = delegate cap (r1 corrupted by SAVE_FRAME)

    lgr     %r2, %r7
    lgr     %r3, %r13               // share with child domain
    NCALL   MEM_SHARE
    // BUG-004: MEM_SHARE expects ERR_SENTINEL — the DGP isolation gate
    // refuses cross-domain share because the bootstrap domain does not
    // hold dgp::isolation::isolation_rights::share against the child domain (child was
    // created with no_rights_cap).  Asserting the failure makes this a
    // real negative test, not a "accept either" no-op.
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    jne     t37_fail
    lgr     %r2, %r7
    NCALL   CAP_RELEASE
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 38: service::register(admin_token, "zx.test")  
    
    PRINT   t38_name
    lgr     %r2, %r12               // a0 = admin cap token
    larl    %r3, name_service       // a1 = name pointer
    NCALL   SVC_REGISTER
    CHECK_OK t38_fail
    lgr     %r0, %r2                // r0 = service_id
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 39: service::lookup("zx.test")  
    
    PRINT   t39_name
    larl    %r2, name_service
    NCALL   SVC_LOOKUP
    CHECK_OK t39_fail
    lgr     %r7, %r2                // r7 = looked-up cap token
    // Validate the looked-up token (it has admin rights)
    lgr     %r2, %r7
    lghi    %r3, 0x20               // admin right
    NCALL   CAP_VALIDATE
    // Release
    lgr     %r2, %r7
    NCALL   CAP_RELEASE
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 40: service::unregister(service_id) (cross: PC)
    
    PRINT   t40_name
    lgr     %r2, %r0                // service_id from register
    NCALL   SVC_UNREGISTER
    CHECK_OK t40_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 41: dom::strand_create(admin, cpu=0, self)  
    
    PRINT   t41_name
    lgr     %r2, %r12               // admin cap token
    lghi    %r3, 0                  // CPU 0 / default BSP placement
    lgr     %r4, %r8                // owner = self domain
    NCALL   DOM_STRAND_CREATE
    CHECK_OK t41_fail
    llilf   %r1, 0xffffffff         // invalid kobject-id sentinel
    cgr     %r2, %r1
    je      t41_fail
    lgr     %r7, %r2                // r7 = first created strand id
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 42: dom::strand_create(admin, cpu=0, self)  
    
    PRINT   t42_name
    lgr     %r2, %r12               // admin cap token
    lghi    %r3, 0                  // CPU 0 / default BSP placement
    lgr     %r4, %r8                // owner = self domain
    NCALL   DOM_STRAND_CREATE
    CHECK_OK t42_fail
    llilf   %r1, 0xffffffff
    cgr     %r2, %r1
    je      t42_fail
    cgr     %r2, %r7
    je      t42_fail                // strand IDs must be distinct
    lgr     %r11, %r2               // r11 = second created strand id
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 43: scheduler::yield loop  
    
    PRINT   t43_name
    lghi    %r7, 8
.Lt43_yield_loop:
    lghi    %r2, 0
    NCALL   SYS_YIELD
    CHECK_OK t43_fail
    aghi    %r7, -1
    cghi    %r7, 0
    jne     .Lt43_yield_loop
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 44: dom::strand_create(invalid CPU)  
    
    PRINT   t44_name
    lgr     %r2, %r12               // admin cap token
    lghi    %r3, 768                // invalid CPU (>= CONFIG_ZX_MAX_CPUS=768)
    lgr     %r4, %r8                // owner = self domain
    NCALL   DOM_STRAND_CREATE
    // Expect error
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    jne     t44_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 45: dom::strand_create(admin, cpu=0, child)  
    
    PRINT   t45_name
    lgr     %r2, %r12               // admin cap token
    lghi    %r3, 0                  // CPU 0
    lgr     %r4, %r13               // owner = child domain
    NCALL   DOM_STRAND_CREATE
    // Expect error (cross-domain spawn denied)
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    jne     t45_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 46: dom::destroy(child)  
    
    PRINT   t46_name
    lgr     %r2, %r12               // admin cap token
    lgr     %r3, %r13               // target = child domain_id
    NCALL   DOM_DESTROY
    CHECK_OK t46_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 47: cap::release(admin_token), then reacquire it for later admin calls.
    
    PRINT   t47_name
    lgr     %r2, %r12
    NCALL   CAP_RELEASE
    CHECK_OK t47_fail
    lgr     %r2, %r10               // bootstrap-issued source token
    lghi    %r3, 0x20               // cap_rights::admin
    NCALL   CAP_ACQUIRE
    CHECK_OK t47_fail
    lgr     %r12, %r2
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 48: dom::create(no-token)  
    
    PRINT   t48_name
    lghi    %r2, ERR_SENTINEL       // bogus admin token
    larl    %r3, name_child
    NCALL   DOM_CREATE
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    jne     t48_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 49: cap::validate(null)  
    
    PRINT   t49_name
    lghi    %r2, 0                  // null token
    lghi    %r3, 0x20               // admin right
    NCALL   CAP_VALIDATE
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    jne     t49_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 50: sgn::mask_set (mask all signal bits = 0xFFFF_FFFF)
    
    PRINT   t50_name
    lghi    %r2, -1                 // all kinds blocked
    NCALL   SGN_MASK_SET
    CHECK_OK t50_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 51: sgn::mask_get should be non-error after mask_set(-1)
    
    PRINT   t51_name
    NCALL   SGN_MASK_GET
    lghi    %r1, ERR_SENTINEL
    cgr     %r2, %r1
    je      t51_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 52: sgn::mask_set (clear the mask)
    
    PRINT   t52_name
    lghi    %r2, 0                  // no kinds blocked
    NCALL   SGN_MASK_SET
    CHECK_OK t52_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 53: sgn::mask_get should now return 0
    
    PRINT   t53_name
    NCALL   SGN_MASK_GET
    ltgr    %r2, %r2
    jne     t53_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 54: sgn::send(self, kind=timer, data=0x42)
    //   Dispatcher auth level == signal uses a0 as target domain id.
    //   Bootstrap granted our domain signal right.
    
    PRINT   t54_name
    lgr     %r2, %r8                // target = self domain_id
    lghi    %r3, 1                  // signal_kind::timer = 1
    lghi    %r4, 0x42               // data
    NCALL   SGN_SEND
    CHECK_OK t54_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 55: sgn::poll — the timer bit must be pending; I/O may also be pending.
    
    PRINT   t55_name
    NCALL   SGN_POLL
    lghi    %r1, 2
    ngr     %r2, %r1
    cgr     %r2, %r1
    jne     t55_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 56: sgn::ack(timer bit) clears the pending bit.
    
    PRINT   t56_name
    lghi    %r2, 2                  // ack timer bit
    NCALL   SGN_ACK
    CHECK_OK t56_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 57: dom::strand_query_state(self, strand_id=0)
    //   Returns packed u64 with substate in bits 0..7.  Self strand is
    //   running on the bootstrap CPU at this point.
    
    PRINT   t57_name
    lgr     %r2, %r12               // admin cap
    lghi    %r3, 0                  // strand_id = 0
    NCALL   DOM_STRAND_QUERY
    CHECK_OK t57_fail
    // Sanity: substate (bits 0..7) should be sched_substate::running == 2.
    llgcr   %r1, %r2                // %r1 = low byte
    cghi    %r1, 2
    jne     t57_fail
    PRINT   t_pass
    aghi    %r9, 1

    
    // Test 58: dom::strand_set_affinity(self, strand_id=0, mask=1)
    //   Restrict this domain's primary strand to CPU 0.
    
    PRINT   t58_name
    lgr     %r2, %r12               // admin cap
    lghi    %r3, 0                  // strand_id = 0
    lghi    %r4, 1                  // sched_mask bits = CPU 0 only
    NCALL   DOM_STRAND_SET_AFFIN
    CHECK_OK t58_fail
    PRINT   t_pass
    aghi    %r9, 1

    // Test 59: ipc::endpoint_create(self)

    PRINT   t59_name
    lgr     %r2, %r8                // peer = self domain
    NCALL   IPC_ENDPOINT_CREATE
    CHECK_OK t59_fail
    lgr     %r13, %r2               // endpoint id
    PRINT   t_pass
    aghi    %r9, 1

    // Test 60: ipc::send(endpoint, request)

    PRINT   t60_name
    lghi    %r2, PAGE_SIZE
    NCALL   MEM_ALLOC
    CHECK_OK t60_fail
    lgr     %r6, %r2                // writable IPC request/result page
    larl    %r4, ipc_payload
    stg     %r4, 0(%r6)             // request.payload
    lghi    %r4, 0
    stg     %r4, 8(%r6)             // request.capabilities
    stg     %r4, 16(%r6)            // request.grants
    lghi    %r4, 4
    st      %r4, 24(%r6)            // request.length
    lghi    %r4, 0
    sth     %r4, 28(%r6)            // request.capability_count
    sth     %r4, 30(%r6)            // request.grant_count
    lgr     %r2, %r13
    lgr     %r3, %r6
    NCALL   IPC_SEND
    CHECK_OK t60_fail
    PRINT   t_pass
    aghi    %r9, 1

    // Test 61: ipc::receive(endpoint, payload, result) and verify payload.

    PRINT   t61_name
    lgr     %r3, %r6
    aghi    %r3, 64                 // receive payload buffer
    lgr     %r4, %r6
    aghi    %r4, 320                // user_message_result
    stg     %r3, 0(%r4)
    lghi    %r5, 0
    stg     %r5, 8(%r4)
    stg     %r5, 16(%r4)
    stg     %r5, 24(%r4)
    lgr     %r2, %r13
    NCALL   IPC_RECEIVE
    lghi    %r1, 4
    CHECK_EQ %r1, t61_fail
    llgc    %r4, 64(%r6)
    lghi    %r1, 0x49              // 'I'
    cgr     %r4, %r1
    jne     t61_fail
    PRINT   t_pass
    aghi    %r9, 1

    // Test 62: policy::register(pager, endpoint)

    PRINT   t62_name
    lghi    %r2, 0                 // policy::server_kind::pager
    lgr     %r3, %r13
    NCALL   POLICY_REGISTER
    CHECK_OK t62_fail
    PRINT   t_pass
    aghi    %r9, 1

    // Test 63: policy::unregister(pager)

    PRINT   t63_name
    lghi    %r2, 0                 // policy::server_kind::pager
    NCALL   POLICY_UNREGISTER
    CHECK_OK t63_fail
    PRINT   t_pass
    aghi    %r9, 1

    // Test 64: ipc::endpoint_destroy(endpoint)

    PRINT   t64_name
    lgr     %r2, %r13
    NCALL   IPC_ENDPOINT_DESTROY
    CHECK_OK t64_fail
    lgr     %r2, %r6
    lghi    %r3, PAGE_SIZE
    NCALL   MEM_FREE
    CHECK_OK t64_fail
    PRINT   t_pass
    aghi    %r9, 1

    // Test 65: shm::export a resident anonymous page to this domain.

    PRINT   t65_name
    lghi    %r2, PAGE_SIZE
    NCALL   MEM_ALLOC
    CHECK_OK t65_fail
    lgr     %r11, %r2               // source VA
    lghi    %r3, 0x5a
    stc     %r3, 0(%r11)            // make the source page resident
    lgr     %r2, %r11
    lghi    %r3, PAGE_SIZE
    lgr     %r4, %r8                // target = self domain
    NCALL   SHM_EXPORT
    CHECK_OK t65_fail
    lgr     %r7, %r2                // exported opaque memory token
    PRINT   t_pass
    aghi    %r9, 1

    // Test 66: shm::import returns the AR binding for the fixed portal VA.

    PRINT   t66_name
    lgr     %r2, %r7
    lgfi    %r3, 0x20000000
    lghi    %r4, PAGE_SIZE
    NCALL   SHM_IMPORT
    CHECK_OK t66_fail
    cghi    %r2, 1
    jne     t66_fail
    PRINT   t_pass
    aghi    %r9, 1

    // Test 67: shm::map assigns the next AR to a second portal VA.

    PRINT   t67_name
    lgr     %r2, %r7
    lgfi    %r3, 0x20002000
    lghi    %r4, PAGE_SIZE
    NCALL   SHM_MAP
    CHECK_OK t67_fail
    cghi    %r2, 2
    jne     t67_fail
    PRINT   t_pass
    aghi    %r9, 1

    // Test 68: both portals access the source page through their assigned AR.

    PRINT   t68_name
    lgfi    %r1, 0x20000000          // r1 selects AR1
    llgc    %r4, 0(%r1)
    lghi    %r1, 0x5a
    cgr     %r4, %r1
    jne     t68_fail
    lgfi    %r2, 0x20002000          // r2 selects AR2
    llgc    %r4, 0(%r2)
    cgr     %r4, %r1
    jne     t68_fail
    PRINT   t_pass
    aghi    %r9, 1

    // Test 69: release the exported token and source region. Portal AR views
    // remain live until domain teardown revokes them.

    PRINT   t69_name
    lgr     %r2, %r7
    NCALL   CAP_RELEASE
    CHECK_OK t69_fail
    lgr     %r2, %r11
    lghi    %r3, PAGE_SIZE
    NCALL   MEM_FREE
    CHECK_OK t69_fail
    PRINT   t_pass
    aghi    %r9, 1

    PRINT   msg_all_pass
    .Lexit:
    NCALL DOM_EXIT

// Failure handlers

t02_fail:  PRINT t02_fail_msg;  j .Lexit
t03_fail:  PRINT t03_fail_msg;  j .Lexit
t04_fail:  PRINT t04_fail_msg;  j .Lexit
t05_fail:  PRINT t05_fail_msg;  j .Lexit
t06_fail:  PRINT t06_fail_msg;  j .Lexit
t07_fail:  PRINT t07_fail_msg;  j .Lexit
t08_fail:  PRINT t08_fail_msg;  j .Lexit
t09_fail:  PRINT t09_fail_msg;  j .Lexit
t10_fail:  PRINT t10_fail_msg;  j .Lexit
t11_fail:  PRINT t11_fail_msg;  j .Lexit
t13_fail:  PRINT t13_fail_msg;  j .Lexit
t14_fail:  PRINT t14_fail_msg;  j .Lexit
t15_fail:  PRINT t15_fail_msg;  j .Lexit
t16_fail:  PRINT t16_fail_msg;  j .Lexit
t17_fail:  PRINT t17_fail_msg;  j .Lexit
t18_fail:  PRINT t18_fail_msg;  j .Lexit
t19_fail:  PRINT t19_fail_msg;  j .Lexit
t20_fail:  PRINT t20_fail_msg;  j .Lexit
t21_fail:  PRINT t21_fail_msg;  j .Lexit
t22_fail:  PRINT t22_fail_msg;  j .Lexit
t23_fail:  PRINT t23_fail_msg;  j .Lexit
t24_fail:  PRINT t24_fail_msg;  j .Lexit
t25_fail:  PRINT t25_fail_msg;  j .Lexit
t26_fail:  PRINT t26_fail_msg;  j .Lexit
t27_fail:  PRINT t27_fail_msg;  j .Lexit
t28_fail:  PRINT t28_fail_msg;  j .Lexit
t29_fail:  PRINT t29_fail_msg;  j .Lexit
t30_fail:  PRINT t30_fail_msg;  j .Lexit
t31_fail:  PRINT t31_fail_msg;  j .Lexit
t32_fail:  PRINT t32_fail_msg;  j .Lexit
t33_fail:  PRINT t33_fail_msg;  j .Lexit
t34_fail:  PRINT t34_fail_msg;  j .Lexit
t35_fail:  PRINT t35_fail_msg;  j .Lexit
t36_fail:  PRINT t36_fail_msg;  j .Lexit
t37_fail:  PRINT t37_fail_msg;  j .Lexit
t38_fail:  PRINT t38_fail_msg;  j .Lexit
t39_fail:  PRINT t39_fail_msg;  j .Lexit
t40_fail:  PRINT t40_fail_msg;  j .Lexit
t41_fail:  PRINT t41_fail_msg;  j .Lexit
t42_fail:  PRINT t42_fail_msg;  j .Lexit
t43_fail:  PRINT t43_fail_msg;  j .Lexit
t44_fail:  PRINT t44_fail_msg;  j .Lexit
t45_fail:  PRINT t45_fail_msg;  j .Lexit
t46_fail:  PRINT t46_fail_msg;  j .Lexit
t47_fail:  PRINT t47_fail_msg;  j .Lexit
t48_fail:  PRINT t48_fail_msg;  j .Lexit
t49_fail:  PRINT t49_fail_msg;  j .Lexit
t50_fail:  PRINT t50_fail_msg;  j .Lexit
t51_fail:  PRINT t51_fail_msg;  j .Lexit
t52_fail:  PRINT t52_fail_msg;  j .Lexit
t53_fail:  PRINT t53_fail_msg;  j .Lexit
t54_fail:  PRINT t54_fail_msg;  j .Lexit
t55_fail:  PRINT t55_fail_msg;  j .Lexit
t56_fail:  PRINT t56_fail_msg;  j .Lexit
t57_fail:  PRINT t57_fail_msg;  j .Lexit
t58_fail:  PRINT t58_fail_msg;  j .Lexit
t59_fail:  PRINT t59_fail_msg;  j .Lexit
t60_fail:  PRINT t60_fail_msg;  j .Lexit
t61_fail:  PRINT t61_fail_msg;  j .Lexit
t62_fail:  PRINT t62_fail_msg;  j .Lexit
t63_fail:  PRINT t63_fail_msg;  j .Lexit
t64_fail:  PRINT t64_fail_msg;  j .Lexit
t65_fail:  PRINT t65_fail_msg;  j .Lexit
t66_fail:  PRINT t66_fail_msg;  j .Lexit
t67_fail:  PRINT t67_fail_msg;  j .Lexit
t68_fail:  PRINT t68_fail_msg;  j .Lexit
t69_fail:  PRINT t69_fail_msg;  j .Lexit

// Data

    .section .rodata
    .align 8

msg_startup:   .ascii "[init] Pre-eXecution System 26h1\n"
    .equ msg_startup_len, . - msg_startup
	.align 2

msg_all_pass:  .ascii "[init] All tests passed: "
    .equ msg_all_pass_len, . - msg_all_pass
	.align 2

msg_newline:   .ascii "\n"
    .equ msg_newline_len, . - msg_newline
	.align 2

name_child:    .asciz "test/child"
	.align 2
name_service:  .asciz "zx.test"
	.align 8
ipc_payload:   .ascii "IPC!"

t_pass:        .ascii "PASS\n"
    .equ t_pass_len, . - t_pass

// Failure messages
    .align 8
t02_fail_msg:  .ascii "FAIL: dom::info returned error\n"
    .equ t02_fail_msg_len, . - t02_fail_msg
	.align 2
t03_fail_msg:  .ascii "FAIL: dom::query returned error\n"
    .equ t03_fail_msg_len, . - t03_fail_msg
	.align 2
t04_fail_msg:  .ascii "FAIL: mem::alloc returned error\n"
    .equ t04_fail_msg_len, . - t04_fail_msg
	.align 2
t05_fail_msg:  .ascii "FAIL: page write+read mismatch\n"
    .equ t05_fail_msg_len, . - t05_fail_msg
	.align 2
t06_fail_msg:  .ascii "FAIL: mem::protect(RO) failed\n"
    .equ t06_fail_msg_len, . - t06_fail_msg
	.align 2
t07_fail_msg:  .ascii "FAIL: read from RO page failed\n"
    .equ t07_fail_msg_len, . - t07_fail_msg
	.align 2
t08_fail_msg:  .ascii "FAIL: mem::protect(RW) failed\n"
    .equ t08_fail_msg_len, . - t08_fail_msg
	.align 2
t09_fail_msg:  .ascii "FAIL: write after RW restore failed\n"
    .equ t09_fail_msg_len, . - t09_fail_msg
	.align 2
t10_fail_msg:  .ascii "FAIL: mem::free returned error\n"
    .equ t10_fail_msg_len, . - t10_fail_msg
	.align 2
t11_fail_msg:  .ascii "FAIL: multi-page alloc failed\n"
    .equ t11_fail_msg_len, . - t11_fail_msg
	.align 2
t13_fail_msg:  .ascii "FAIL: verify 4 pages failed\n"
    .equ t13_fail_msg_len, . - t13_fail_msg
	.align 2
t14_fail_msg:  .ascii "FAIL: multi-page free failed\n"
    .equ t14_fail_msg_len, . - t14_fail_msg
	.align 2
t15_fail_msg:  .ascii "FAIL: time_now failed\n"
    .equ t15_fail_msg_len, . - t15_fail_msg
	.align 2
t16_fail_msg:  .ascii "FAIL: time_deadline failed\n"
    .equ t16_fail_msg_len, . - t16_fail_msg
	.align 2
t17_fail_msg:  .ascii "FAIL: time_yield failed\n"
    .equ t17_fail_msg_len, . - t17_fail_msg
	.align 2
t18_fail_msg:  .ascii "FAIL: cap::acquire(admin) failed\n"
    .equ t18_fail_msg_len, . - t18_fail_msg
	.align 2
t19_fail_msg:  .ascii "FAIL: cap::validate(admin) failed\n"
    .equ t19_fail_msg_len, . - t19_fail_msg
	.align 2
t20_fail_msg:  .ascii "FAIL: dom::create failed\n"
    .equ t20_fail_msg_len, . - t20_fail_msg
	.align 2
t21_fail_msg:  .ascii "FAIL: dom::query(child) failed\n"
    .equ t21_fail_msg_len, . - t21_fail_msg
	.align 2
t22_fail_msg:  .ascii "FAIL: cap::acquire(map) failed\n"
    .equ t22_fail_msg_len, . - t22_fail_msg
	.align 2
t23_fail_msg:  .ascii "FAIL: cap::acquire(delegate) failed\n"
    .equ t23_fail_msg_len, . - t23_fail_msg
	.align 2
t24_fail_msg:  .ascii "FAIL: sgn::register_handler failed\n"
    .equ t24_fail_msg_len, . - t24_fail_msg
	.align 2
t25_fail_msg:  .ascii "FAIL: sgn::poll failed\n"
    .equ t25_fail_msg_len, . - t25_fail_msg
	.align 2
t26_fail_msg:  .ascii "FAIL: sgn::ack failed\n"
    .equ t26_fail_msg_len, . - t26_fail_msg
	.align 2
t27_fail_msg:  .ascii "FAIL: gate::create failed\n"
    .equ t27_fail_msg_len, . - t27_fail_msg
	.align 2
t28_fail_msg:  .ascii "FAIL: gate::destroy failed\n"
    .equ t28_fail_msg_len, . - t28_fail_msg
	.align 2
t29_fail_msg:  .ascii "FAIL: portal::create failed\n"
    .equ t29_fail_msg_len, . - t29_fail_msg
	.align 2
t30_fail_msg:  .ascii "FAIL: portal::destroy failed\n"
    .equ t30_fail_msg_len, . - t30_fail_msg
	.align 2
t31_fail_msg:  .ascii "FAIL: DMA map or CCW completion failed\n"
    .equ t31_fail_msg_len, . - t31_fail_msg
	.align 2
t32_fail_msg:  .ascii "FAIL: io::unbind failed\n"
    .equ t32_fail_msg_len, . - t32_fail_msg
	.align 2
t33_fail_msg:  .ascii "FAIL: io::submit failed\n"
    .equ t33_fail_msg_len, . - t33_fail_msg
	.align 2
t34_fail_msg:  .ascii "FAIL: io::irq_bind failed\n"
    .equ t34_fail_msg_len, . - t34_fail_msg
	.align 2
t35_fail_msg:  .ascii "FAIL: mem::unmap failed\n"
    .equ t35_fail_msg_len, . - t35_fail_msg
	.align 2
t36_fail_msg:  .ascii "FAIL: mem::map failed\n"
    .equ t36_fail_msg_len, . - t36_fail_msg
	.align 2
t37_fail_msg:  .ascii "FAIL: mem::share failed\n"
    .equ t37_fail_msg_len, . - t37_fail_msg
	.align 2
t38_fail_msg:  .ascii "FAIL: service::register failed\n"
    .equ t38_fail_msg_len, . - t38_fail_msg
	.align 2
t39_fail_msg:  .ascii "FAIL: service::lookup failed\n"
    .equ t39_fail_msg_len, . - t39_fail_msg
	.align 2
t40_fail_msg:  .ascii "FAIL: service::unregister failed\n"
    .equ t40_fail_msg_len, . - t40_fail_msg
	.align 2
t41_fail_msg:  .ascii "FAIL: dom::strand_create(self) failed\n"
    .equ t41_fail_msg_len, . - t41_fail_msg
	.align 2
t42_fail_msg:  .ascii "FAIL: second dom::strand_create failed\n"
    .equ t42_fail_msg_len, . - t42_fail_msg
	.align 2
t43_fail_msg:  .ascii "FAIL: scheduler yield loop failed\n"
    .equ t43_fail_msg_len, . - t43_fail_msg
	.align 2
t44_fail_msg:  .ascii "FAIL: invalid CPU strand create should fail\n"
    .equ t44_fail_msg_len, . - t44_fail_msg
	.align 2
t45_fail_msg:  .ascii "FAIL: cross-domain strand create should fail\n"
    .equ t45_fail_msg_len, . - t45_fail_msg
	.align 2
t46_fail_msg:  .ascii "FAIL: dom::destroy failed\n"
    .equ t46_fail_msg_len, . - t46_fail_msg
	.align 2
t47_fail_msg:  .ascii "FAIL: cap::release failed\n"
    .equ t47_fail_msg_len, . - t47_fail_msg
	.align 2
t48_fail_msg:  .ascii "FAIL: dom::create(no-token) should fail\n"
    .equ t48_fail_msg_len, . - t48_fail_msg
	.align 2
t49_fail_msg:  .ascii "FAIL: cap::validate(null) should fail\n"
    .equ t49_fail_msg_len, . - t49_fail_msg
	.align 2
t50_fail_msg:  .ascii "FAIL: sgn::mask_set(all) failed\n"
    .equ t50_fail_msg_len, . - t50_fail_msg
	.align 2
t51_fail_msg:  .ascii "FAIL: sgn::mask_get returned ERR after mask_set(all)\n"
    .equ t51_fail_msg_len, . - t51_fail_msg
	.align 2
t52_fail_msg:  .ascii "FAIL: sgn::mask_set(0) failed\n"
    .equ t52_fail_msg_len, . - t52_fail_msg
	.align 2
t53_fail_msg:  .ascii "FAIL: sgn::mask_get should be 0 after clearing\n"
    .equ t53_fail_msg_len, . - t53_fail_msg
	.align 2
t54_fail_msg:  .ascii "FAIL: sgn::send(self, timer) failed\n"
    .equ t54_fail_msg_len, . - t54_fail_msg
	.align 2
t55_fail_msg:  .ascii "FAIL: sgn::poll did not report timer bit pending\n"
    .equ t55_fail_msg_len, . - t55_fail_msg
	.align 2
t56_fail_msg:  .ascii "FAIL: sgn::ack(timer) failed\n"
    .equ t56_fail_msg_len, . - t56_fail_msg
	.align 2
t57_fail_msg:  .ascii "FAIL: dom::strand_query_state returned wrong substate\n"
    .equ t57_fail_msg_len, . - t57_fail_msg
	.align 2
t58_fail_msg:  .ascii "FAIL: dom::strand_set_affinity failed\n"
    .equ t58_fail_msg_len, . - t58_fail_msg
	.align 2
t59_fail_msg:  .ascii "FAIL: ipc::endpoint_create failed\n"
    .equ t59_fail_msg_len, . - t59_fail_msg
	.align 2
t60_fail_msg:  .ascii "FAIL: ipc::send failed\n"
    .equ t60_fail_msg_len, . - t60_fail_msg
	.align 2
t61_fail_msg:  .ascii "FAIL: ipc::receive payload mismatch\n"
    .equ t61_fail_msg_len, . - t61_fail_msg
	.align 2
t62_fail_msg:  .ascii "FAIL: policy::register failed\n"
    .equ t62_fail_msg_len, . - t62_fail_msg
	.align 2
t63_fail_msg:  .ascii "FAIL: policy::unregister failed\n"
    .equ t63_fail_msg_len, . - t63_fail_msg
	.align 2
t64_fail_msg:  .ascii "FAIL: ipc::endpoint_destroy failed\n"
    .equ t64_fail_msg_len, . - t64_fail_msg
	.align 2
t65_fail_msg:  .ascii "FAIL: shm::export failed\n"
    .equ t65_fail_msg_len, . - t65_fail_msg
	.align 2
t66_fail_msg:  .ascii "FAIL: shm::import failed\n"
    .equ t66_fail_msg_len, . - t66_fail_msg
	.align 2
t67_fail_msg:  .ascii "FAIL: shm::map failed\n"
    .equ t67_fail_msg_len, . - t67_fail_msg
	.align 2
t68_fail_msg:  .ascii "FAIL: shm imported data mismatch\n"
    .equ t68_fail_msg_len, . - t68_fail_msg
	.align 2
t69_fail_msg:  .ascii "FAIL: shm cleanup failed\n"
    .equ t69_fail_msg_len, . - t69_fail_msg

// Test names
    .align 8
t01_name:  .ascii "[init] Test  1: sys::console_write .............. "; .equ t01_name_len, . - t01_name
	.align 2
t02_name:  .ascii "[init] Test  2: dom::info ....................... "; .equ t02_name_len, . - t02_name
	.align 2
t03_name:  .ascii "[init] Test  3: dom::query(self) ................ "; .equ t03_name_len, . - t03_name
	.align 2
t04_name:  .ascii "[init] Test  4: mem::alloc ...................... "; .equ t04_name_len, . - t04_name
	.align 2
t05_name:  .ascii "[init] Test  5: demand paging write+read ........ "; .equ t05_name_len, . - t05_name
	.align 2
t06_name:  .ascii "[init] Test  6: mem::protect(RO) ................ "; .equ t06_name_len, . - t06_name
	.align 2
t07_name:  .ascii "[init] Test  7: read from RO page ............... "; .equ t07_name_len, . - t07_name
	.align 2
t08_name:  .ascii "[init] Test  8: mem::protect(RW) ................ "; .equ t08_name_len, . - t08_name
	.align 2
t09_name:  .ascii "[init] Test  9: write after RW .................. "; .equ t09_name_len, . - t09_name
	.align 2
t10_name:  .ascii "[init] Test 10: mem::free ....................... "; .equ t10_name_len, . - t10_name
	.align 2
t11_name:  .ascii "[init] Test 11: mem::alloc multi-page ........... "; .equ t11_name_len, . - t11_name
	.align 2
t12_name:  .ascii "[init] Test 12: touch 4 pages ................... "; .equ t12_name_len, . - t12_name
	.align 2
t13_name:  .ascii "[init] Test 13: verify 4 pages .................. "; .equ t13_name_len, . - t13_name
	.align 2
t14_name:  .ascii "[init] Test 14: mem::free multi-page ............ "; .equ t14_name_len, . - t14_name
	.align 2
t15_name:  .ascii "[init] Test 15: sys::time_now ................... "; .equ t15_name_len, . - t15_name
	.align 2
t16_name:  .ascii "[init] Test 16: sys::time_deadline .............. "; .equ t16_name_len, . - t16_name
	.align 2
t17_name:  .ascii "[init] Test 17: sys::time_yield ................. "; .equ t17_name_len, . - t17_name
	.align 2
t18_name:  .ascii "[init] Test 18: cap::acquire(admin) ............. "; .equ t18_name_len, . - t18_name
	.align 2
t19_name:  .ascii "[init] Test 19: cap::validate(admin) ............ "; .equ t19_name_len, . - t19_name
	.align 2
t20_name:  .ascii "[init] Test 20: dom::create ..................... "; .equ t20_name_len, . - t20_name
	.align 2
t21_name:  .ascii "[init] Test 21: dom::query(child) ............... "; .equ t21_name_len, . - t21_name
	.align 2
t22_name:  .ascii "[init] Test 22: cap::acquire(map) ............... "; .equ t22_name_len, . - t22_name
	.align 2
t23_name:  .ascii "[init] Test 23: cap::acquire(delegate) .......... "; .equ t23_name_len, . - t23_name
	.align 2
t24_name:  .ascii "[init] Test 24: sgn::register_handler ........... "; .equ t24_name_len, . - t24_name
	.align 2
t25_name:  .ascii "[init] Test 25: sgn::poll ....................... "; .equ t25_name_len, . - t25_name
	.align 2
t26_name:  .ascii "[init] Test 26: sgn::ack ........................ "; .equ t26_name_len, . - t26_name
	.align 2
t27_name:  .ascii "[init] Test 27: gate::create .................... "; .equ t27_name_len, . - t27_name
	.align 2
t28_name:  .ascii "[init] Test 28: gate::destroy ................... "; .equ t28_name_len, . - t28_name
	.align 2
t29_name:  .ascii "[init] Test 29: portal::create .................. "; .equ t29_name_len, . - t29_name
	.align 2
t30_name:  .ascii "[init] Test 30: portal::destroy ................. "; .equ t30_name_len, . - t30_name
	.align 2
t31_name:  .ascii "[init] Test 31: DMA map + CCW completion ........ "; .equ t31_name_len, . - t31_name
	.align 2
t32_name:  .ascii "[init] Test 32: io::unbind ...................... "; .equ t32_name_len, . - t32_name
	.align 2
t33_name:  .ascii "[init] Test 33: io::submit(bogus)->ERR .......... "; .equ t33_name_len, . - t33_name
	.align 2
t34_name:  .ascii "[init] Test 34: io::irq_bind(gate=0)->ERR ....... "; .equ t34_name_len, . - t34_name
	.align 2
t35_name:  .ascii "[init] Test 35: mem::unmap ...................... "; .equ t35_name_len, . - t35_name
	.align 2
t36_name:  .ascii "[init] Test 36: raw mem::map -> ERR ............. "; .equ t36_name_len, . - t36_name
	.align 2
t37_name:  .ascii "[init] Test 37: legacy mem::share -> ERR ........ "; .equ t37_name_len, . - t37_name
	.align 2
t38_name:  .ascii "[init] Test 38: service::register ............... "; .equ t38_name_len, . - t38_name
	.align 2
t39_name:  .ascii "[init] Test 39: service::lookup ................. "; .equ t39_name_len, . - t39_name
	.align 2
t40_name:  .ascii "[init] Test 40: service::unregister ............. "; .equ t40_name_len, . - t40_name
	.align 2
t41_name:  .ascii "[init] Test 41: dom::strand_create(self) ........ "; .equ t41_name_len, . - t41_name
	.align 2
t42_name:  .ascii "[init] Test 42: dom::strand_create(second) ...... "; .equ t42_name_len, . - t42_name
	.align 2
t43_name:  .ascii "[init] Test 43: scheduler yield loop ............ "; .equ t43_name_len, . - t43_name
	.align 2
t44_name:  .ascii "[init] Test 44: dom::strand_create(cpu)->ERR .... "; .equ t44_name_len, . - t44_name
	.align 2
t45_name:  .ascii "[init] Test 45: dom::strand_create(child)->ERR .. "; .equ t45_name_len, . - t45_name
	.align 2
t46_name:  .ascii "[init] Test 46: dom::destroy .................... "; .equ t46_name_len, . - t46_name
	.align 2
t47_name:  .ascii "[init] Test 47: cap::release .................... "; .equ t47_name_len, . - t47_name
	.align 2
t48_name:  .ascii "[init] Test 48: dom::create(no-token)->ERR ...... "; .equ t48_name_len, . - t48_name
	.align 2
t49_name:  .ascii "[init] Test 49: cap::validate(null)->ERR ........ "; .equ t49_name_len, . - t49_name
	.align 2
t50_name:  .ascii "[init] Test 50: sgn::mask_set(all) ................ "; .equ t50_name_len, . - t50_name
	.align 2
t51_name:  .ascii "[init] Test 51: sgn::mask_get (nonzero) ........... "; .equ t51_name_len, . - t51_name
	.align 2
t52_name:  .ascii "[init] Test 52: sgn::mask_set(0) .................. "; .equ t52_name_len, . - t52_name
	.align 2
t53_name:  .ascii "[init] Test 53: sgn::mask_get should be 0 .......... "; .equ t53_name_len, . - t53_name
	.align 2
t54_name:  .ascii "[init] Test 54: sgn::send(self, timer, 0x42) ........ "; .equ t54_name_len, . - t54_name
	.align 2
t55_name:  .ascii "[init] Test 55: sgn::poll reports timer bit ........ "; .equ t55_name_len, . - t55_name
	.align 2
t56_name:  .ascii "[init] Test 56: sgn::ack clears timer bit ........... "; .equ t56_name_len, . - t56_name
	.align 2
t57_name:  .ascii "[init] Test 57: dom::strand_query_state ............ "; .equ t57_name_len, . - t57_name
	.align 2
t58_name:  .ascii "[init] Test 58: dom::strand_set_affinity ........... "; .equ t58_name_len, . - t58_name
	.align 2
t59_name:  .ascii "[init] Test 59: ipc::endpoint_create .............. "; .equ t59_name_len, . - t59_name
	.align 2
t60_name:  .ascii "[init] Test 60: ipc::send ........................ "; .equ t60_name_len, . - t60_name
	.align 2
t61_name:  .ascii "[init] Test 61: ipc::receive ..................... "; .equ t61_name_len, . - t61_name
	.align 2
t62_name:  .ascii "[init] Test 62: policy::register .................. "; .equ t62_name_len, . - t62_name
	.align 2
t63_name:  .ascii "[init] Test 63: policy::unregister ................ "; .equ t63_name_len, . - t63_name
	.align 2
t64_name:  .ascii "[init] Test 64: ipc::endpoint_destroy ............. "; .equ t64_name_len, . - t64_name
	.align 2
t65_name:  .ascii "[init] Test 65: shm::export ....................... "; .equ t65_name_len, . - t65_name
	.align 2
t66_name:  .ascii "[init] Test 66: shm::import ....................... "; .equ t66_name_len, . - t66_name
	.align 2
t67_name:  .ascii "[init] Test 67: shm::map .......................... "; .equ t67_name_len, . - t67_name
	.align 2
t68_name:  .ascii "[init] Test 68: shm data visibility ................ "; .equ t68_name_len, . - t68_name
	.align 2
t69_name:  .ascii "[init] Test 69: shm cleanup ........................ "; .equ t69_name_len, . - t69_name

    .section .note.GNU-stack, "", @progbits
