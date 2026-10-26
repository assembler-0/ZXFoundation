/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/features.zcomponent/features.cxx
/// @brief CPU facility registry

module arch.s390x.cpu.features;
import arch.s390x.cpu.ctlreg;
import zxfoundation.base.types;
import zxfoundation.sys.zinit.core;
import lib.cstring;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace arch::s390x::cpu::features {

    /// Internal facility list cache — same size as the STFLE maximum.
    no_export std::array<u64, STFLE_MAX_DWORDS> g_fac_list{};

    auto init_list(std::optional<const u64*> stfle_fac) noexcept -> void {
        if (!stfle_fac) discard_value detect<STFLE_MAX_DWORDS>(g_fac_list);
        else
            lib::memcpy(
                g_fac_list.data(),
                stfle_fac.value(),
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
        cr0.set<"cpu_timer_ssm">(true);
        if (has(facility::asn_lx_reuse))
            cr0.set<"asn_lx_reuse_ctl">(true);
        if (has(facility::instruction_exec_protection))
            cr0.set<"inst_exec_prot_enablement_ctl">(true);
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

    auto zi_cpu_features_init(const zxfoundation::sys::zinit::zi_desc&) noexcept
        -> zxfoundation::sys::zinit::zi_result
    {
        init_list(std::nullopt);
        enable_hardware_facilities();
        return {};
    }
    __zinit__(early, p60, zi_cpu_features_init, fatal)


} // namespace arch::s390x::cpu::features
