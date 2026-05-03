// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/atomic.h
//
/// @brief s390x atomic operations and memory barriers.
///
///        All atomic ops are built on CS (Compare-and-Swap, 32-bit) and
///        CSG (Compare-and-Swap, 64-bit).  On s390x, CS/CSG are the
///        architectural primitives — there is no LL/SC.
///
///        BARRIER NOTES
///        =============
///        s390x has a Total Store Order (TSO) memory model with one
///        exception: stores can be reordered with respect to subsequent
///        loads (store-load reordering).  BCR 14,0 (serialization) is
///        the full barrier.  CS/CSG themselves imply a full barrier on
///        the accessing CPU, but NOT on other CPUs — hence explicit
///        barriers are still required around lock acquire/release.

#pragma once

#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// Memory barriers
// ---------------------------------------------------------------------------

/// @brief Full memory barrier (serialization instruction).
///        BCR 14,0 is the canonical s390x serialization fence.
///        It prevents all load/store reordering across the barrier on
///        the issuing CPU and ensures prior stores are visible to all CPUs
///        before any subsequent load or store is issued.
#define mb()    __asm__ volatile ("bcr 14,0" ::: "memory")

/// @brief Compiler-only barrier — prevents the compiler from reordering
///        memory accesses across this point, but emits no hardware fence.
///        Use when the hardware ordering is already guaranteed (e.g. inside
///        a CS loop) but you need to stop the compiler from hoisting loads.
#define barrier() __asm__ volatile ("" ::: "memory")

/// @brief Acquire barrier: all loads/stores after this point see all
///        stores that happened before the paired release on another CPU.
///        On s390x TSO, loads are not reordered with prior loads/stores,
///        so a compiler barrier suffices for the acquire side.
#define smp_mb_acquire()    barrier()

/// @brief Release barrier: all loads/stores before this point are visible
///        to other CPUs before any subsequent store.  On s390x TSO, a
///        store-load fence (BCR 14,0) is needed to prevent store-load
///        reordering across the release point.
#define smp_mb_release()    mb()

/// @brief Full SMP barrier.
#define smp_mb()            mb()

// ---------------------------------------------------------------------------
// atomic_t  (32-bit)
// ---------------------------------------------------------------------------

typedef struct { volatile int32_t val; } atomic_t;

#define ATOMIC_INIT(v)  { .val = (v) }

/// @brief Implementations are in arch/s390x/cpu/atomic.c
int32_t  atomic_read(const atomic_t *a);
void     atomic_set(atomic_t *a, int32_t v);
int32_t  atomic_add_return(atomic_t *a, int32_t delta);
int32_t  atomic_sub_return(atomic_t *a, int32_t delta);
/// @brief Atomically compare *a with old; if equal, store new. Returns old value.
int32_t  atomic_cmpxchg(atomic_t *a, int32_t old_val, int32_t new_val);
/// @brief Atomically exchange *a with new_val. Returns previous value.
int32_t  atomic_xchg(atomic_t *a, int32_t new_val);

static inline void    atomic_inc(atomic_t *a)       { atomic_add_return(a,  1); }
static inline void    atomic_dec(atomic_t *a)       { atomic_add_return(a, -1); }
static inline int32_t atomic_inc_return(atomic_t *a){ return atomic_add_return(a,  1); }
static inline int32_t atomic_dec_return(atomic_t *a){ return atomic_add_return(a, -1); }
/// @brief Returns true if the value after decrement is zero.
static inline bool    atomic_dec_and_test(atomic_t *a) { return atomic_dec_return(a) == 0; }

// ---------------------------------------------------------------------------
// atomic64_t  (64-bit)
// ---------------------------------------------------------------------------

typedef struct { volatile int64_t val; } atomic64_t;

#define ATOMIC64_INIT(v) { .val = (v) }

int64_t  atomic64_read(const atomic64_t *a);
void     atomic64_set(atomic64_t *a, int64_t v);
int64_t  atomic64_add_return(atomic64_t *a, int64_t delta);
int64_t  atomic64_sub_return(atomic64_t *a, int64_t delta);
int64_t  atomic64_cmpxchg(atomic64_t *a, int64_t old_val, int64_t new_val);
int64_t  atomic64_xchg(atomic64_t *a, int64_t new_val);

static inline void    atomic64_inc(atomic64_t *a)        { atomic64_add_return(a,  1); }
static inline void    atomic64_dec(atomic64_t *a)        { atomic64_add_return(a, -1); }
static inline int64_t atomic64_inc_return(atomic64_t *a) { return atomic64_add_return(a,  1); }
static inline int64_t atomic64_dec_return(atomic64_t *a) { return atomic64_add_return(a, -1); }
static inline bool    atomic64_dec_and_test(atomic64_t *a){ return atomic64_dec_return(a) == 0; }
