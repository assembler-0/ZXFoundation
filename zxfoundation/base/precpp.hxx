/// SPDX-License-Identifier: Apache-2.0
/// @file precpp.hxx
/// @brief Validate header

#pragma once

#if !defined(__ASSMBLER__) || !defined(__ASSEMBLY__)

#define __stringify(x) #x
#define stringify(x)   __stringify(x)

#define FMT_MSG(m) "zxfoundation: precpp.hxx - " m

#if !defined(__zxfoundation__)
#error FMT_MSG("not compiling for ZXFoundation")
#endif

#if !defined(__s390__) || !defined(__s390x__) || !defined(__zarch__)
#error FMT_MSG("not compiling for s390x/z/Arch")
#endif

#define real __real__
#define imaginary __imag__

#define nonnull _Nonnull
#define nullable _Nullable
#define nullunspec _Null_unspecified

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

#if defined(__cpp_contracts) && __cpp_contracts >= 202400L
    #define assert(expr) contract_assert(expr)
#else
#define assert(expr) \
    do { if (!(expr)) [[unlikely]] zxfoundation::sys::syschk::syschk_fatal(lib::kernel_error::from_generic(lib::generic_error::internal_error), #expr); } while (0)
#endif

#if 0

#define CC_IPM(sym)		"	ipm	%[" __stringify(sym) "]\n"
#define CC_OUT(sym, var)	[sym] "=d" (var)
#define CC_TRANSFORM(cc)	({ (cc) >> 28; })
#define CC_CLOBBER		"cc"
#define CC_CLOBBER_LIST(...)	"cc", __VA_ARGS__

#endif

#endif
