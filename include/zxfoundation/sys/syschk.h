// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sys/syschk.h
//
/// @brief ZXFoundation System Check (syschk) subsystem.
///
///        The halt path acquires no locks, calls no kernel subsystems,
///        and dereferences no kernel data structures.  It is safe to call
///        from any context: exception handlers, IRQ handlers, early init,
///        or a corrupt-memory state.

#pragma once

#include <zxfoundation/types.h>
#include <arch/s390x/init/zxfl/zxfl.h>

// ---------------------------------------------------------------------------
// Code encoding
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

#define ZX_SYSCHK_CLASS_FATAL       0xFu
#define ZX_SYSCHK_CLASS_CRITICAL    0xCu
#define ZX_SYSCHK_CLASS_WARNING     0x3u

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------

#define ZX_SYSCHK_DOMAIN_CORE   0x0u
#define ZX_SYSCHK_DOMAIN_MEM    0x1u
#define ZX_SYSCHK_DOMAIN_SYNC   0x2u
#define ZX_SYSCHK_DOMAIN_ARCH   0x3u
#define ZX_SYSCHK_DOMAIN_SCHED  0x4u
#define ZX_SYSCHK_DOMAIN_IO     0x5u

// ---------------------------------------------------------------------------
// Code table — FATAL
// ---------------------------------------------------------------------------

#define ZX_SYSCHK_CORE_CORRUPT          ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x01)
#define ZX_SYSCHK_CORE_UNINITIALIZED    ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x02)
#define ZX_SYSCHK_CORE_ASSERT           ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x03)
#define ZX_SYSCHK_CORE_UNREACHABLE      ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x04)
#define ZX_SYSCHK_CORE_INTERNAL_ERROR   ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x05)
#define ZX_SYSCHK_CORE_STACK_CORRUPT    ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x06)
#define ZX_SYSCHK_CORE_BAD_LOADER       ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_CORE, 0x07)

#define ZX_SYSCHK_MEM_OOM               ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_MEM,  0x01)
#define ZX_SYSCHK_MEM_CORRUPT           ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_MEM,  0x02)
#define ZX_SYSCHK_MEM_DOUBLE_FREE       ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_MEM,  0x03)
#define ZX_SYSCHK_MEM_USE_AFTER_FREE    ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_MEM,  0x04)
#define ZX_SYSCHK_MEM_OVERRUN           ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_MEM,  0x05)

#define ZX_SYSCHK_SYNC_DEADLOCK         ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_SYNC, 0x01)
#define ZX_SYSCHK_SYNC_BAD_UNLOCK       ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_SYNC, 0x02)

#define ZX_SYSCHK_ARCH_BAD_PSW          ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_ARCH, 0x01)
#define ZX_SYSCHK_ARCH_UNHANDLED_TRAP   ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_ARCH, 0x02)
#define ZX_SYSCHK_ARCH_MCHECK           ZX_SYSCHK_CODE(ZX_SYSCHK_CLASS_FATAL, ZX_SYSCHK_DOMAIN_ARCH, 0x03)

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
// Crash record
//
// Written to a fixed offset inside the BSP lowcore pad region (0x1400)
// before halting.  The lowcore is always mapped and accessible regardless
// of kernel state.  The record is read post-mortem by a debugger or
// operator console.
// ---------------------------------------------------------------------------

#define ZX_CRASH_RECORD_MAGIC   0x5A584352554E4348ULL  /* "ZXCRUNCH" */
#define ZX_CRASH_RECORD_OFFSET  0x1400u                /* within lowcore */
#define ZX_CRASH_MSG_LEN        128u

typedef struct __attribute__((packed)) {
    uint64_t magic;                     ///< ZX_CRASH_RECORD_MAGIC
    uint16_t code;                      ///< zx_syschk_code_t
    uint8_t  _pad[6];
    uint64_t psw_mask;                  ///< PSW mask at time of syschk
    uint64_t psw_addr;                  ///< PSW address at time of syschk
    char     msg[ZX_CRASH_MSG_LEN];     ///< NUL-terminated reason string
} zx_crash_record_t;

_Static_assert(sizeof(zx_crash_record_t) <= 256,
               "zx_crash_record_t must fit in 256 bytes");

typedef uint16_t zx_syschk_code_t;

/// @brief Initialize the system check subsystem.
/// @param boot    Pointer to the boot protocol
void zx_syschk_initialize(const zxfl_boot_protocol_t *boot);

/// @brief Issue a system check.
///        All severity classes halt unconditionally.
///        Acquires no locks.  Safe from any context.
/// @param code  zx_syschk_code_t encoding class, domain, and type.
/// @param fmt   printf-style format string.
/// @param ...   Format arguments.
[[noreturn]] void zx_system_check(zx_syschk_code_t code, const char *fmt, ...);
