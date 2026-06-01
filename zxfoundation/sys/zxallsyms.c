/// SPDX-License-Identifier: Apache-2.0
/// @file zxallsyms.c
/// @brief ZXFoundation All-Symbols Table — runtime lookup.

#include <zxfoundation/sys/zxallsyms.h>
#include <lib/string.h>

extern const uint64_t zxallsyms_addresses[];
extern const uint32_t zxallsyms_offsets[];
extern const char     zxallsyms_names[];
extern const uint32_t zxallsyms_num_syms;

/// @brief Return the NUL-terminated name for symbol index i.
static inline const char *sym_name(uint32_t i) {
    return &zxallsyms_names[zxallsyms_offsets[i]];
}

/// @brief Copy at most (size-1) bytes of src into dst and NUL-terminate.
///        Does nothing if dst is NULL or size is 0.
/// @param dst Destination buffer.
/// @param size Destination buffer size.
/// @param src Source string.
static void safe_copy_name(char *dst, size_t size, const char *src) {
    if (!dst || size == 0)
        return;
    size_t i = 0;
    while (i < size - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

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
                           uint64_t *offset) {
    const uint32_t n = zxallsyms_num_syms;
    if (n == 0)
        return false;

    if (addr < zxallsyms_addresses[0])
        return false;

    uint32_t lo = 0;
    uint32_t hi = n - 1;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (zxallsyms_addresses[mid] <= addr)
            lo = mid;
        else
            hi = mid - 1;
    }

    // lo is now the index of the nearest symbol at or before addr.
    safe_copy_name(name_out, size, sym_name(lo));
    if (offset)
        *offset = addr - zxallsyms_addresses[lo];

    return true;
}

/// @brief Resolve a symbol name to its kernel virtual address.
///
///        Performs a linear scan of the name blob.  O(N) — intended for
///        one-shot use (debugger, init-time patching), not hot paths.
///
/// @param[in] name      NUL-terminated symbol name to search for.
/// @param[out] addr_out  Receives the symbol's virtual address on success.
/// @return true if found; false otherwise.
bool zxallsyms_lookup_name(const char *name, uint64_t *addr_out) {
    if (!name)
        return false;

    const uint32_t n = zxallsyms_num_syms;
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(sym_name(i), name) == 0) {
            if (addr_out)
                *addr_out = zxallsyms_addresses[i];
            return true;
        }
    }
    return false;
}

/// @brief Iterate over all symbols in ascending address order.
///
/// @param[in] cb   Callback invoked for each symbol.
/// @param[in] arg  Opaque argument forwarded to cb unchanged.
void zxallsyms_for_each(zxallsyms_iter_fn cb, void *arg) {
    if (!cb)
        return;
    const uint32_t n = zxallsyms_num_syms;
    for (uint32_t i = 0; i < n; i++) {
        if (!cb(zxallsyms_addresses[i], sym_name(i), arg))
            return;
    }
}

/// @brief Return the total number of symbols in the table.
uint32_t zxallsyms_count(void) {
    return zxallsyms_num_syms;
}
