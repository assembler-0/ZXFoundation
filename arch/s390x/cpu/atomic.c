// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/atomic.c
//
/// @brief s390x atomic operations using CS (32-bit) and CSG (64-bit).
///
///        CS/CSG SEMANTICS
///        ================
///        CS  R1, R3, D2(B2)  — if mem[D2+B2] == R1, store R3 → mem; else load mem → R1.
///        CSG R1, R3, D2(B2)  — same, 64-bit.
///        CC=0: swap succeeded.  CC=1: swap failed (R1 updated with current value).
///
///        The retry loop ("jo 0b") retries on CC=1.  This is the canonical
///        s390x read-modify-write pattern.  There is no exponential backoff
///        here — the kernel's critical sections are short and contention is
///        expected to be low.  If we ever need backoff, add it here.

#include <arch/s390x/cpu/atomic.h>
#include <arch/s390x/cpu/processor.h>

// ---------------------------------------------------------------------------
// atomic_t (32-bit, CS)
// ---------------------------------------------------------------------------

int32_t atomic_read(const atomic_t *a) {
    // Plain volatile load — no CS needed for a read.
    // The volatile on the field prevents the compiler from caching the value,
    // but we still need a barrier if ordering with respect to other CPUs matters;
    // callers are responsible for that (use smp_mb() if needed).
    return a->val;
}

void atomic_set(atomic_t *a, int32_t v) {
    // Plain volatile store.  Callers must issue smp_mb() if they need
    // the store to be ordered with respect to a subsequent lock release.
    a->val = v;
}

int32_t atomic_add_return(atomic_t *a, int32_t delta) {
    int32_t old, new_val;
    __asm__ volatile (
        "0: l   %[old], %[mem]\n"       // load current value
        "   lr  %[new], %[old]\n"       // new = old
        "   ar  %[new], %[d]\n"         // new += delta
        "   cs  %[old], %[new], %[mem]\n" // if mem==old: mem=new; else old=mem
        "   jo  0b\n"                   // retry on failure (CC=1)
        : [old] "=&d" (old),
          [new] "=&d" (new_val),
          [mem] "+Q"  (a->val)
        : [d]   "d"   (delta)
        : "cc", "memory"
    );
    return new_val;
}

int32_t atomic_sub_return(atomic_t *a, int32_t delta) {
    return atomic_add_return(a, -delta);
}

int32_t atomic_cmpxchg(atomic_t *a, int32_t old_val, int32_t new_val) {
    __asm__ volatile (
        "cs %[old], %[new], %[mem]\n"
        : [old] "+d"  (old_val),
          [mem] "+Q"  (a->val)
        : [new] "d"   (new_val)
        : "cc", "memory"
    );
    return old_val; // CS updates old_val with the actual value on failure
}

int32_t atomic_xchg(atomic_t *a, int32_t new_val) {
    int32_t old;
    __asm__ volatile (
        "0: l   %[old], %[mem]\n"
        "   cs  %[old], %[new], %[mem]\n"
        "   jo  0b\n"
        : [old] "=&d" (old),
          [mem] "+Q"  (a->val)
        : [new] "d"   (new_val)
        : "cc", "memory"
    );
    return old;
}

// ---------------------------------------------------------------------------
// atomic64_t (64-bit, CSG)
// ---------------------------------------------------------------------------

int64_t atomic64_read(const atomic64_t *a) {
    return a->val;
}

void atomic64_set(atomic64_t *a, int64_t v) {
    a->val = v;
}

int64_t atomic64_add_return(atomic64_t *a, int64_t delta) {
    int64_t old, new_val;
    __asm__ volatile (
        "0: lg  %[old], %[mem]\n"
        "   lgr %[new], %[old]\n"
        "   agr %[new], %[d]\n"
        "   csg %[old], %[new], %[mem]\n"
        "   jo  0b\n"
        : [old] "=&d" (old),
          [new] "=&d" (new_val),
          [mem] "+QS" (a->val)
        : [d]   "d"   (delta)
        : "cc", "memory"
    );
    return new_val;
}

int64_t atomic64_sub_return(atomic64_t *a, int64_t delta) {
    return atomic64_add_return(a, -delta);
}

int64_t atomic64_cmpxchg(atomic64_t *a, int64_t old_val, int64_t new_val) {
    __asm__ volatile (
        "csg %[old], %[new], %[mem]\n"
        : [old] "+d"  (old_val),
          [mem] "+QS" (a->val)
        : [new] "d"   (new_val)
        : "cc", "memory"
    );
    return old_val;
}

int64_t atomic64_xchg(atomic64_t *a, int64_t new_val) {
    int64_t old;
    __asm__ volatile (
        "0: lg  %[old], %[mem]\n"
        "   csg %[old], %[new], %[mem]\n"
        "   jo  0b\n"
        : [old] "=&d" (old),
          [mem] "+QS" (a->val)
        : [new] "d"   (new_val)
        : "cc", "memory"
    );
    return old;
}
