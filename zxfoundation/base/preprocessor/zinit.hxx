/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/base/preprocessor/zinit.hxx
/// @brief zinit macro support — descriptor emission into .zinit_data
/// @note requires __zxmacro_want_zinit=1

#pragma once

/// @brief Emit a baseline init descriptor.
/// @param c      zi_class token (prelink, early, mm, core, fs, bus, device, late).
/// @param p      zi_prio  token (highest, high, normal, low, lowest).
/// @param entry  Function symbol: auto (const zi_desc&) noexcept -> std::expected<void, lib::kernel_error>.
/// @param a      zi_action token (warn, fatal, silent, bitmask expression).
/// @note No dependencies and no entailed successors are declared.  Ordering is
///       determined by class, runtime prio sort, then link order.
#define __zinit__(c, p, entry, a)                                                   \
    extern "C" {                                                                  \
    constexpr zxfoundation::sys::zinit::zi_desc __zi_desc_##entry                   \
    [[gnu::used, gnu::section(".zinit_data." stringify(c) "." stringify(entry))]]   \
    {                                                                               \
        .name    = stringify(entry),                                                \
        .init    = entry,                                                           \
        .cls     = zxfoundation::sys::zinit::zi_class::c,                           \
        .prio    = zxfoundation::sys::zinit::zi_prio::p,                            \
        .action  = zxfoundation::sys::zinit::zi_action::a,                          \
        .deps    = nullptr,                                                         \
        .entails = nullptr,                                                         \
    };                                                                              \
    }

/// @brief Emit an init descriptor with explicit dependency names.
/// @param c      zi_class token.
/// @param p      zi_prio  token.
/// @param entry  Function symbol.
/// @param a      zi_action token.
/// @param ...    Comma-separated string-literal names that must run BEFORE
///               this entry.  Stored as a NULL-terminated static array.
/// @note The node runs only after every named dependency completes
///       successfully.  Unresolved names fault at graph build time.
#define __zinit_dep__(c, p, entry, a, ...)                                          \
    extern "C" {                                                                  \
    static constexpr const char* const __zi_deps_##entry[] = { __VA_ARGS__, nullptr }; \
    constexpr zxfoundation::sys::zinit::zi_desc __zi_desc_##entry                   \
    [[gnu::used, gnu::section(".zinit_data." stringify(c) "." stringify(entry))]]   \
    {                                                                               \
        .name    = stringify(entry),                                                \
        .init    = entry,                                                           \
        .cls     = zxfoundation::sys::zinit::zi_class::c,                           \
        .prio    = zxfoundation::sys::zinit::zi_prio::p,                            \
        .action  = zxfoundation::sys::zinit::zi_action::a,                          \
        .deps    = __zi_deps_##entry,                                               \
        .entails = nullptr,                                                         \
    };                                                                              \
    }

/// @brief Emit an init descriptor with entailed successor names.
/// @param c      zi_class token.
/// @param p      zi_prio  token.
/// @param entry  Function symbol.
/// @param a      zi_action token.
/// @param ...    Comma-separated string-literal names that must run AFTER
///               this entry.  Stored as a NULL-terminated static array.
/// @note "Entail" is the inverse of "depend on": this entry guarantees it
///       precedes every named node.  Unresolved names fault at graph build.
#define __zinit_entail__(c, p, entry, a, ...)                                       \
    extern "C" {                                                                  \
    static constexpr const char* const __zi_entails_##entry[] = { __VA_ARGS__, nullptr }; \
    constexpr zxfoundation::sys::zinit::zi_desc __zi_desc_##entry                   \
    [[gnu::used, gnu::section(".zinit_data." stringify(c) "." stringify(entry))]]   \
    {                                                                               \
        .name    = stringify(entry),                                                \
        .init    = entry,                                                           \
        .cls     = zxfoundation::sys::zinit::zi_class::c,                           \
        .prio    = zxfoundation::sys::zinit::zi_prio::p,                            \
        .action  = zxfoundation::sys::zinit::zi_action::a,                          \
        .deps    = nullptr,                                                         \
        .entails = __zi_entails_##entry,                                            \
    };                                                                              \
    }
