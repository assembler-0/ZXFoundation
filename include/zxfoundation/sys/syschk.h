// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sys/syschk.h
//
/// @brief ZXFoundation System Check (syschk) subsystem.

#pragma once

#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// Code encoding helpers
// ---------------------------------------------------------------------------

#define ZX_SYSCHK_CLASS_SHIFT   12u
#define ZX_SYSCHK_DOMAIN_SHIFT   8u
#define ZX_SYSCHK_TYPE_MASK     0x00FFu

#define ZX_SYSCHK_CODE(cls, dom, typ) \
    ((uint16_t)(((cls) << ZX_SYSCHK_CLASS_SHIFT) | \
                ((dom) << ZX_SYSCHK_DOMAIN_SHIFT) | \
                ((typ) & ZX_SYSCHK_TYPE_MASK)))

#define ZX_SYSCHK_CLASS(code)   (((code) >> ZX_SYSCHK_CLASS_SHIFT) & 0xFu)
#define ZX_SYSCHK_DOMAIN(code)  (((code) >> ZX_SYSCHK_DOMAIN_SHIFT) & 0xFu)
#define ZX_SYSCHK_TYPE(code)    ((code) & ZX_SYSCHK_TYPE_MASK)

// ---------------------------------------------------------------------------
// Severity classes
// ---------------------------------------------------------------------------

#define ZX_SYSCHK_CLASS_FATAL       0xFu  ///< Unrecoverable; always halts.
#define ZX_SYSCHK_CLASS_CRITICAL    0xCu  ///< Severe; always halts.
#define ZX_SYSCHK_CLASS_WARNING     0x3u  ///< Recoverable; filter may suppress.

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------

#define ZX_SYSCHK_DOMAIN_CORE   0x0u  ///< Core kernel / init
#define ZX_SYSCHK_DOMAIN_MEM    0x1u  ///< Memory subsystem
#define ZX_SYSCHK_DOMAIN_SYNC   0x2u  ///< Synchronization primitives
#define ZX_SYSCHK_DOMAIN_ARCH   0x3u  ///< Architecture / hardware
#define ZX_SYSCHK_DOMAIN_SCHED  0x4u  ///< Scheduler
#define ZX_SYSCHK_DOMAIN_IO     0x5u  ///< I/O subsystem

// ---------------------------------------------------------------------------
// Code table — FATAL
// ---------------------------------------------------------------------------

/// Core
#define ZX_SYSCHK_CORE_CORRUPT          ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x01)
#define ZX_SYSCHK_CORE_UNINITIALIZED    ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x02)
#define ZX_SYSCHK_CORE_ASSERT           ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x03)
#define ZX_SYSCHK_CORE_UNREACHABLE      ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x04)
#define ZX_SYSCHK_CORE_INTERNAL_ERROR   ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x05)
#define ZX_SYSCHK_CORE_STACK_CORRUPT    ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x06)
#define ZX_SYSCHK_CORE_BAD_LOADER       ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x07)

/// Memory
#define ZX_SYSCHK_MEM_OOM               ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_MEM,  0x01)
#define ZX_SYSCHK_MEM_CORRUPT           ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_MEM,  0x02)
#define ZX_SYSCHK_MEM_DOUBLE_FREE       ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_MEM,  0x03)
#define ZX_SYSCHK_MEM_USE_AFTER_FREE    ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_MEM,  0x04)
#define ZX_SYSCHK_MEM_OVERRUN           ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_MEM,  0x05)

/// Sync
#define ZX_SYSCHK_SYNC_DEADLOCK         ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_SYNC, 0x01)
#define ZX_SYSCHK_SYNC_BAD_UNLOCK       ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_SYNC, 0x02)

/// Arch
#define ZX_SYSCHK_ARCH_BAD_PSW          ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_ARCH, 0x01)
#define ZX_SYSCHK_ARCH_UNHANDLED_TRAP   ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_ARCH, 0x02)
#define ZX_SYSCHK_ARCH_MCHECK           ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_ARCH, 0x03)

/// Sched
#define ZX_SYSCHK_SCHED_CORRUPT         ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_SCHED, 0x01)

// ---------------------------------------------------------------------------
// Code table — CRITICAL
// ---------------------------------------------------------------------------

#define ZX_SYSCHK_CORE_CRIT_INVARIANT   ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_CRITICAL, ZX_SYSCHK_DOMAIN_CORE, 0x01)
#define ZX_SYSCHK_MEM_CRIT_SLAB        ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_CRITICAL, ZX_SYSCHK_DOMAIN_MEM,  0x01)
#define ZX_SYSCHK_ARCH_CRIT_SMP        ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_CRITICAL, ZX_SYSCHK_DOMAIN_ARCH, 0x01)

// ---------------------------------------------------------------------------
// Code table — WARNING
// ---------------------------------------------------------------------------

#define ZX_SYSCHK_CORE_WARN_DEPRECATED  ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_WARNING, ZX_SYSCHK_DOMAIN_CORE, 0x01)
#define ZX_SYSCHK_MEM_WARN_LOW          ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_WARNING, ZX_SYSCHK_DOMAIN_MEM,  0x01)
#define ZX_SYSCHK_IO_WARN_TIMEOUT       ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_WARNING, ZX_SYSCHK_DOMAIN_IO,   0x01)

// ---------------------------------------------------------------------------
// Filter return values
// ---------------------------------------------------------------------------

#define ZX_SYSCHK_SUPPRESS  0   ///< Suppress halt; execution continues.
#define ZX_SYSCHK_HALT      1   ///< Proceed with halt sequence.

typedef uint16_t zx_syschk_code_t;

/// @brief WARNING-class filter function.
///        Contract: must be async-signal-safe (no locks, no allocation).
///        Never called for FATAL or CRITICAL codes.
/// @param code  The system check code being evaluated.
/// @param msg   Formatted message string (read-only).
/// @return ZX_SYSCHK_SUPPRESS or ZX_SYSCHK_HALT.
typedef int (*zx_syschk_filter_fn)(zx_syschk_code_t code, const char *msg);

/// @brief Issue a system check.
///        FATAL/CRITICAL: tears down SMP, halts unconditionally.
///        WARNING: consults registered filter; halts only if filter returns
///                 ZX_SYSCHK_HALT or no filter is registered.
/// @param code  zx_syschk_code_t encoding class, domain, and type.
/// @param fmt   printf-style format string.
/// @param ...   Format arguments.
void zx_system_check(zx_syschk_code_t code, const char *fmt, ...);

/// @brief Register a WARNING-class filter.
///        Pass NULL to clear.  Not thread-safe; call before SMP bringup.
/// @param fn  Filter function pointer, or NULL.
void zx_system_check_set_filter(zx_syschk_filter_fn fn);
