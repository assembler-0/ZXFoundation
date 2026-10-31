/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/zxallsyms.zcomponent/core.cxx
/// @brief Global kernel symbol table — address/name lookup over a static
///        read-only table generated at link time by zxallsyms_gen.

module zxfoundation.sys.zxallsyms.core;
import zxfoundation.base.types;
import lib.error;
import lib.stl;

extern "C" {
    extern const u64  zxallsyms_addrs[];   ///< Sorted ascending virtual addresses.
    extern const u32  zxallsyms_offsets[]; ///< Byte offset into zxallsyms_names[].
    extern const char zxallsyms_names[];   ///< Flat NUL-terminated name pool.
    extern const u32  zxallsyms_count;     ///< Total number of symbols.
}

namespace zxfoundation::sys::zxallsyms {

    /// @brief Look up the symbol whose range contains @p addr.
    /// @param[in] addr  Virtual address to resolve.
    /// @return sym_info on success.
    [[nodiscard]] auto lookup_by_addr(const u64 addr)
        noexcept -> std::expected<sym_info, lib::kernel_error> {
        const u32 count = zxallsyms_count;
        if (count == 0) [[unlikely]]
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));

        u32 lo = 0;
        u32 hi = count;
        while (lo + 1 < hi) {
            const u32 mid = lo + (hi - lo) / 2u;
            if (zxallsyms_addrs[mid] <= addr)
                lo = mid;
            else
                hi = mid;
        }

        if (zxallsyms_addrs[lo] > addr)
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));

        return sym_info {
            .address = zxallsyms_addrs[lo],
            .name    = &zxallsyms_names[zxallsyms_offsets[lo]],
        };
    }

    /// @brief Look up the first symbol whose demangled name equals @p name.
    /// @param[in] name  Demangled symbol name to search for.
    /// @return sym_info on success.
    [[nodiscard]] auto lookup_by_name(const std::string_view name)
        noexcept -> std::expected<sym_info, lib::kernel_error> {
        if (name.empty()) [[unlikely]]
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));

        const u32 count = zxallsyms_count;
        for (u32 i = 0; i < count; ++i) {
            const char* entry_ptr = &zxallsyms_names[zxallsyms_offsets[i]];
            if (name == std::string_view{entry_ptr}) {
                return sym_info{
                    .address = zxallsyms_addrs[i],
                    .name    = entry_ptr,
                };
            }
        }

        return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
    }

} // namespace zxfoundation::sys::zxallsyms
