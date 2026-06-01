/// SPDX-License-Identifier: Apache-2.0
/// @file zxallsyms.h
/// @brief ZXFoundation All-Symbols Table (zxallsyms).

#pragma once

#include <zxfoundation/types.h>

/// @brief Maximum symbol name length (including NUL terminator).
#define ZXALLSYMS_NAME_MAX  256U

/// @brief Resolve a kernel virtual address to the nearest preceding symbol.
///
///        Performs a binary search on the sorted address table.  Returns the
///        symbol whose base address is the largest value <= addr.
///
/// @param[in] addr      Virtual address to resolve.
/// @param[out] name_out  Buffer to receive the NUL-terminated symbol name.
///                  May be NULL if only the offset is needed.
/// @param[in] size      Capacity of name_out in bytes.  If the name is longer
///                  than size-1 it is truncated; a NUL is always written.
/// @param[in] offset    Receives (addr - symbol_base).  May be NULL.
/// @return true if a symbol was found; false if addr is below all symbols.
bool zxallsyms_lookup_addr(uint64_t addr, char *name_out, size_t size,
                           uint64_t *offset);

/// @brief Resolve a symbol name to its kernel virtual address.
///
///        Performs a linear scan of the name blob.  O(N) — intended for
///        one-shot use (debugger, init-time patching), not hot paths.
///
/// @param[in] name      NUL-terminated symbol name to search for.
/// @param[out] addr_out  Receives the symbol's virtual address on success.
/// @return true if found; false otherwise.
bool zxallsyms_lookup_name(const char *name, uint64_t *addr_out);

/// @brief Callback type for zxallsyms_for_each().
/// @return Return false to stop iteration early; true to continue.
typedef bool (*zxallsyms_iter_fn)(uint64_t addr, const char *name, void *arg);

/// @brief Iterate over all symbols in ascending address order.
///
/// @param[in] cb   Callback invoked for each symbol.
/// @param[in] arg  Opaque argument forwarded to cb unchanged.
void zxallsyms_for_each(zxallsyms_iter_fn cb, void *arg);

/// @brief Return the total number of symbols in the table.
uint32_t zxallsyms_count(void);
