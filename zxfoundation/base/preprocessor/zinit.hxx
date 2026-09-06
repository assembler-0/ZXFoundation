/// SPDX-License-Identifier: Apache-2.0
/// @file zinit.hxx
/// @brief zinit macro support
/// @note requires __zxmacro_want_zinit=1

#pragma once

/// @brief declares zinit entry
/// @note requires @c zxfoundation.sys.zinit.core
#define __zinit__(c, p, entry, a)                                                 \
    declscope(C) {                                                                \
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