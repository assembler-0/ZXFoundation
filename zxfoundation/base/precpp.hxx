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
    #define ZX_HAS_PRE 1
#else
    #define pre(expr)
#endif

#if defined(__cpp_contracts) && __cpp_contracts >= 202400L
    #define ZX_HAS_POST 1
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

#define __internal__ static
#define __external__ extern "C"

#define __endian__(endian) \
    static_assert(__BYTE_ORDER__ == concat(__ORDER_##endian,_ENDIAN__), stringify(__FILE_NAME__) ": requested endian " stringify(concat(__ORDER_##endian,_ENDIAN__)) " but got " stringify(__BYTE_ORDER__))

#define __zinit__(c, p, entry, a)                                                 \
    extern "C" {                                                                  \
    constexpr zxfoundation::sys::zinit::zi_desc __zi_desc_##entry                 \
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

#define bitsfield(size, name) ubit_int<size> name : (size)
#define bitfield(name) bitsfield(1, name)
#define bitrsvd(size) bitsfield(size, concat(__bitrsvd_, __COUNTER__))
#define bytersvd(size) u8 concat(__bytersvd_, __COUNTER__)[size]

#define assert_size(s, sz) \
    static_assert(sizeof(s) == (sz), stringify(s) " != " stringify(sz));

#if defined(__cpp_contracts) && __cpp_contracts >= 202400L
    #define assert(expr) contract_assert(expr)
#else
#define assert(expr) \
    do { if (!(expr)) [[unlikely]] zxfoundation::sys::syschk::syschk_fatal(lib::kernel_error::from_generic(lib::generic_error::internal_error), stringify(expr)); } while (0)
#endif

#if 0

#define CC_IPM(sym)		                \
    "	ipm	%[" stringify(sym) "]\n"    \
    "	srl %[" stringify(sym) "],28\n"
#define CC_OUT(sym, var)	[sym] "=d" (var)
#define CC_TRANSFORM(cc)	({ (cc); })
#define CC_CLOBBER		"cc"
#define CC_CLOBBER_LIST(...)	CC_CLOBBER, __VA_ARGS__

#endif
