/// SPDX-License-Identifier: Apache-2.0
/// @file zxallsyms_stub.cxx
/// @brief Global kernel symbol table stub

using u64 = unsigned long long;
using u32 = unsigned int;

extern "C" {
    extern const u64  zxallsyms_addrs[]   = {0};  ///< Sorted ascending virtual addresses.
    extern const u32  zxallsyms_offsets[] = {0};  ///< Byte offset into zxallsyms_names[].
    extern const char zxallsyms_names[]   = {0}; ///< Flat NUL-terminated name pool.
    extern const u32  zxallsyms_count     = 0;   ///< Total number of symbols.
}
