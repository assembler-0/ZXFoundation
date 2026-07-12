/// SPDX-License-Identifier: Apache-2.0
/// @file precpp.hxx
/// @brief Validate header

#pragma once

#define __stringify(x) #x
#define stringify(x)   __stringify(x)

#define FMT_MSG(m) "zxfoundation: precpp.hxx - " m

#if !defined(__zxfoundation__)
#error FMT_MSG("not compiling for ZXFoundation")
#endif

#if !defined(__s390__) || !defined(__s390x__) || !defined(__zarch__)
#error FMT_MSG("not compiling for s390x/z/Arch")
#endif

#if !defined(_LP64) || !defined(__LP64__)
#error FMT_MSG("not compiling for LP64")
#endif

#if defined(__cpp_contracts) && __cpp_contracts >= 202400L
    #define ZX_HAS_PRE 1
    #define comply(expr) pre(expr)
#else
    #define comply(expr)
#endif

#if defined(__cpp_contracts) && __cpp_contracts >= 202400L
    #define ZX_HAS_POST 1
    #define meet(expr) post(expr)
#else
    #define meet(expr)
#endif

/// @brief exception table: maps faulting instruction addresses
///        to recovery labels for get_user/put_user/copy_from_user patterns.
///        Type field encodes recovery action:
///          0  EX_TYPE_FIXUP       — generic fixup (set PSW to fixup_addr)
///          1  EX_TYPE_UA_LOAD_REG — load rN from fault_addr, N in data[15:12]
///          2  EX_TYPE_UA_LOAD_RP  — load rN/rN+1 pair, N in data[15:12]
///          3  EX_TYPE_UA_STORE    — zero rN, N in data[15:12]
///          4  EX_TYPE_UA_MVCOS_TO — MVCOS copy-to-user; data = reg_rN<<12|len
///          5  EX_TYPE_UA_MVCOS_FROM — MVCOS copy-from-user; data = reg_rN<<12|len
#define EX_TABLE(fault_label, fixup_label, type, data)                       \
    ".pushsection .ex_table, \"a\", @progbits\n"                             \
    ".align  4\n"                                                            \
    ".quad   " stringify(fault_label) "\n"                                   \
    ".long   " stringify(fixup_label) "\n"                                   \
    ".short  " stringify(type) "\n"                                          \
    ".short  " stringify(data) "\n"                                          \
    ".popsection\n"

#define EX_TYPE_FIXUP        0
#define EX_TYPE_UA_LOAD_REG  1   
#define EX_TYPE_UA_LOAD_RP   2   
#define EX_TYPE_UA_STORE     3   
#define EX_TYPE_UA_MVCOS_TO  4   
#define EX_TYPE_UA_MVCOS_FROM 5

#if defined(__cpp_contracts) && __cpp_contracts >= 202400L
    #define assert(expr) contract_assert(expr)
#else
#define assert(expr) \
    do { if (!(expr)) [[unlikely]] zxfoundation::sys::syschk::syschk_fatal(lib::kernel_error::from_generic(lib::generic_error::internal_error), stringify(expr)); } while (0)
#endif

#define CC_IPM(sym)		                \
    "	ipm	%[" stringify(sym) "]\n"    \
    "	srl %[" stringify(sym) "],28\n"
#define CC_OUT(sym, var)	[sym] "=d" (var)
#define CC_TRANSFORM(cc)	({ (cc); })
#define CC_CLOBBER		"cc"
#define CC_CLOBBER_LIST(...)	CC_CLOBBER, __VA_ARGS__

