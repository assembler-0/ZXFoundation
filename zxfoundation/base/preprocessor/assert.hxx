/// SPDX-License-Identifier: Apache-2.0
/// @file assert.hxx
/// @brief Runtime/Static assertion support
/// @note requires __zxmacro_want_assert=1

#pragma once

/// @brief quick endian check, endian={BIG,MIDDLE,SMALL}
#define __endian__(endian) \
    static_assert(__BYTE_ORDER__ == concat(concat(__ORDER_,endian),_ENDIAN__), stringify(__FILE_NAME__) ": requested endian " stringify(concat(concat(__ORDER_,endian),_ENDIAN__)) " but got " stringify(__BYTE_ORDER__))

/// @brief issue system-wide kernel panic (syschk) with sev=fatal, error=generic_error::internal_error
/// @note requires @c zxfoundation.sys.syschk.barebones (no dependencies)
#define __syschk(msg)                                                       \
    zxfoundation::sys::syschk::bug(zxfoundation::sys::syschk::panic_info{   \
        msg, std::source_location::current()                                \
    })

/// @brief lightweight runtime assertion
#define chk_assert(cond) (static_cast<bool>(cond) ? (void)0 : __syschk(stringify(cond)))

/// @brief lightweight runtime check
#define chk_on(cond) (static_cast<bool>(cond) ? __syschk(stringify(cond)) : (void)0)

/// @name quick static size and offset checks
/// @{
#define assert_size(s, sz) \
    static_assert(sizeof(s) == (sz), stringify(s) " != " stringify(sz))

#define assert_offset(t, m, o)  \
    static_assert(__builtin_offsetof(t, m) == (o), "offsetof(" stringify(t) "," stringify(m) ") != " stringify(o))
/// @}