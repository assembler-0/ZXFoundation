/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/features.cxxm
/// @brief Centralized CPU facility registry — every facility bit the kernel
///        cares about, cached from STFLE and queryable via has() / has_raw().

module arch.s390x.cpu.features;
import arch.s390x.cpu.ctlreg;
import zxfoundation.base.types;
import lib.cstring;
import std;

namespace arch::s390x::cpu::features {

    /// Internal facility list cache — same size as the STFLE maximum.
    __internal__ std::array<u64, STFLE_MAX_DWORDS> g_fac_list{};

    auto init(std::optional<const u64*> stfle_fac) noexcept -> void {
        if (!stfle_fac) discard_value detect<STFLE_MAX_DWORDS>(g_fac_list);
        else
            lib::memcpy(
                g_fac_list.data(),              // Destination pointer
                stfle_fac.value(),              // Source pointer
                STFLE_MAX_DWORDS * 8            // Total bytes (DWORDS * 8 bytes each)
            );
    }

    auto has(facility f) noexcept -> bool {
        return has_raw(static_cast<u32>(f));
    }

    auto has_raw(const u32 bit) noexcept -> bool {
        const u32 dword = bit / 64U;
        const u32 pos   = bit % 64U;
        if (dword >= STFLE_MAX_DWORDS)
            return false;
        return (g_fac_list[dword] & (1ULL << (63U - pos))) != 0;
    }

    /// @brief Enable hardware facilities (AFP, Vector) in CR0 for the current CPU.
    auto enable_hardware_facilities() noexcept -> void {
        ctlreg::ctl_set_bit<0>(45);
        if (has(facility::vector))
            ctlreg::ctl_set_bit<0>(46);
        if (has(facility::transactional_exec))
            ctlreg::ctl_set_bit<0>(8);
    }

} // namespace arch::s390x::cpu::features
