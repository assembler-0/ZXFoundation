/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/zxallsyms.zcomponent/storage.cxx
/// @brief Global kernel symbol table stub

using u64 = unsigned long long;
using u32 = unsigned int;

from_global(nomangle) {
    from_global(reference) const u64  zxallsyms_addrs[] = {0};   ///< Sorted ascending virtual addresses.
    from_global(reference) const u32  zxallsyms_offsets[] = {0}; ///< Byte offset into zxallsyms_names[].
    from_global(reference) const char zxallsyms_names[] = {0};   ///< Flat NUL-terminated name pool.
    from_global(reference) const u32  zxallsyms_count = 0;       ///< Total number of symbols.
}