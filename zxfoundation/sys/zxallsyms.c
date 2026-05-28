// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sys/zxallsyms.c
//
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

void zxallsyms_for_each(zxallsyms_iter_fn cb, void *arg) {
    if (!cb)
        return;
    const uint32_t n = zxallsyms_num_syms;
    for (uint32_t i = 0; i < n; i++) {
        if (!cb(zxallsyms_addresses[i], sym_name(i), arg))
            return;
    }
}

uint32_t zxallsyms_count(void) {
    return zxallsyms_num_syms;
}
