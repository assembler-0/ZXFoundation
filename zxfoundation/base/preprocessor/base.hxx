/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/base/preprocessor/base.hxx
/// @brief base macros included in every TU

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

#define discard_value (void)
