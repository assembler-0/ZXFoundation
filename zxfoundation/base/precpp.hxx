/// SPDX-License-Identifier: Apache-2.0
/// @file precpp.hxx
/// @brief Validate header

#pragma once

#define __stringify(x) #x
#define stringify(x)   __stringify(x)

#define __concat(x, y) x##y
#define concat(x, y) __concat(x, y)

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#  if __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#    error "not compiling for big-endian"
#  endif
#elif defined(__BIG_ENDIAN__) || defined(_BIG_ENDIAN)
#else
#  error "not compiling for big-endian"
#endif

#if !defined(__zxfoundation__)
#error "not compiling for ZXFoundation"
#endif

#if !defined(__s390__) && !defined(__s390x__) && !defined(__zarch__)
#error "not compiling for s390x/z/Arch"
#endif

#if !defined(_LP64) || !defined(__LP64__)
#error "not compiling for LP64"
#endif

#if defined(__cpp_contracts) && __cpp_contracts >= 202400L
#else
    #define pre(expr)
#endif

#if defined(__cpp_contracts) && __cpp_contracts >= 202400L
#else
    #define post(expr)
#endif


/// @brief Exception table: maps faulting instruction addresses to recovery
///        labels using signed PC-relative offsets.
#define EX_TABLE(fault_label, fixup_label, type, data)                       \
    ".pushsection .ex_table, \"a\", @progbits\n"                             \
    ".balign 4\n"                                                            \
    ".long   (" stringify(fault_label) ") - .\n"                             \
    ".long   (" stringify(fixup_label) ") - .\n"                             \
    ".short  " stringify(type) "\n"                                          \
    ".short  " stringify(data) "\n"                                          \
    ".popsection\n"

#define EX_TYPE_FIXUP        0
#define EX_TYPE_UA_LOAD_REG  1   
#define EX_TYPE_UA_LOAD_RP   2   
#define EX_TYPE_UA_STORE     3   
#define EX_TYPE_UA_MVCOS_TO  4   
#define EX_TYPE_UA_MVCOS_FROM 5

#define __endian__(endian) \
    static_assert(__BYTE_ORDER__ == concat(concat(__ORDER_,endian),_ENDIAN__), stringify(__FILE_NAME__) ": requested endian " stringify(concat(concat(__ORDER_,endian),_ENDIAN__)) " but got " stringify(__BYTE_ORDER__))

#define __zinit__(c, p, entry, a)                                                 \
    no_mangle {                                                                   \
    constinit zxfoundation::sys::zinit::zi_desc __zi_desc_##entry                 \
    [[gnu::used, gnu::section(".zinit_data." stringify(c) "." stringify(entry))]] \
    {                                                                             \
        .name = stringify(entry),                                                 \
        .init = entry,                                                            \
        .cls  = zxfoundation::sys::zinit::zi_class::c,                            \
        .prio = zxfoundation::sys::zinit::zi_prio::p,                             \
        .action = zxfoundation::sys::zinit::zi_action::a,                         \
    };                                                                            \
    }

#define discard_value (void)
#define no_export static
#define no_mangle extern "C"
#define from_global_reference extern
#define from_global_nomangle extern "C"
#define from_global(T) concat(from_global_,T)
#define declscope(s) extern stringify(s)

#define bitsfield(size, name) ubit_int<size> name : (size)
#define bitfield(name) bitsfield(1, name)
#define bitlimit(expr, bits) expr : bits
#define bitrsvd(size) bitsfield(size, concat(__bitrsvd_,__COUNTER__))
#define bytersvd(size) u8 concat(__bytersvd_,__COUNTER__)[size]

#define trap_assert(cond) (static_cast<bool>(cond) ? (void)0 : __builtin_trap())
#define trap_on(cond) (static_cast<bool>(cond) ? __builtin_trap() : (void)0)

#define __asm_machine_begin() \
    ".machine push\n"
#define __asm_machine(x) \
    ".machine " stringify(x) "\n"
#define __asm_machine_end() \
    ".machine pop\n"

#define __machine__(mac)      \
    __asm__ (                 \
        __asm_machine_begin() \
        __asm_machine(mac)    \
    )

#define __asm_capture_cc()     \
    "ipm  %[__cc__]\n"         \
    "srl  %[__cc__],28\n"
#define __asm_cc_out(c)        \
    [__cc__] "=d" (c)

#define symaliasname(fn) concat(__symalias_,fn)
#define symalias_reference(fn) __asm__(stringify(symaliasname(fn)))
#define symalias(fn) no_mangle auto symaliasname(fn)

/// @name NOTE: this is for System/360 lineup (incl. z/Architecture)
/// @{
#define __b_TO_BYTES(bytes) (bytes)
#define __hw_TO_BYTES(hwords) __b_TO_BYTES((hwords) * 2)
#define __w_TO_BYTES(words)   __hw_TO_BYTES((words) * 2)
#define __dw_TO_BYTES(dwords) __w_TO_BYTES((dwords) * 2)
#define __qw_TO_BYTES(qwords) __dw_TO_BYTES((qwords) * 2)
#define tobytes(type, val) concat(concat(__,type),_TO_BYTES)(val)

#define __b_BITS 7
#define __hw_BITS 15
#define __w_BITS  31
#define __dw_BITS 63
#define ibmbit(type, x) (concat(concat(__,type),_BITS) - (x))
#define ibmbitmask(type, x) (1ULL << (concat(concat(__,type),_BITS) - (x)))
#define ibmbitmask8(x)  ibmbitmask(b, x)
#define ibmbitmask16(x) ibmbitmask(hw, x)
#define ibmbitmask32(x) ibmbitmask(w, x)
#define ibmbitmask64(x) ibmbitmask(dw, x)
/// @}

#define assert_size(s, sz) \
    static_assert(sizeof(s) == (sz), stringify(s) " != " stringify(sz))

#define assert_offset(t, m, o)  \
    static_assert(__builtin_offsetof(t, m) == (o), "offsetof(" stringify(t) "," stringify(m) ") != " stringify(o))