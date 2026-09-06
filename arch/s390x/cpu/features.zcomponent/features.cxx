/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/features.cxxm
/// @brief Centralized CPU facility registry — every facility bit the kernel
///        cares about, cached from STFLE and queryable via has() / has_raw().

module arch.s390x.cpu.features;
import arch.s390x.cpu.ctlreg;
import zxfoundation.base.types;
import lib.cstring;
import lib.stl;

namespace arch::s390x::cpu::features {

    /// Internal facility list cache — same size as the STFLE maximum.
    no_export std::array<u64, STFLE_MAX_DWORDS> g_fac_list{};

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
        ctlreg::cr0 cr0{};
        ctlreg::crread<0, 0>(cr0);
        cr0.set<"afp_register_control">(true);
        cr0.set<"secondary_space_control">(true);
        cr0.set<"clock_comparator_ssm">(true);
        if (has(facility::vector))
            cr0.set<"vector_enablement_control">(true);
        if (has(facility::transactional_exec))
            cr0.set<"transactional_exec_control">(true);
        ctlreg::crwrite<0, 0>(cr0);

        ctlreg::cr3 cr3{};
        cr3.set<"psw_key_mask">(0xFFFF);

        ctlreg::cr4 cr4{};
        cr4.set<"auth_index">(0xFFFF);

        ctlreg::crwrite<3, 3>(cr3);
        ctlreg::crwrite<4, 4>(cr4);

        ctlreg::cr6 cr6{};
        ctlreg::crread<6, 6>(cr6);
        cr6.set<"io_interruption_ssm">(0xff);
        ctlreg::crwrite<6, 6>(cr6);

        ctlreg::cr14 cr14{};
        ctlreg::crread<14, 14>(cr14);
        cr14.set<"channel_report_pending_subclass_mask">(true);
        cr14.set<"recovery_subclass_mask">(true);
        cr14.set<"degradation_subclass_mask">(true);
        cr14.set<"external_damage_subclass_mask">(true);
        cr14.set<"warning_subclass_mask">(true);
        ctlreg::crwrite<14, 14>(cr14);
    }

} // namespace arch::s390x::cpu::features
