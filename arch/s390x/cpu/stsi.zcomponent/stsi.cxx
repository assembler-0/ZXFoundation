/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/stsi.zcomponent/stsi.cxx
/// @brief STSI (STore System Information) API

module arch.s390x.cpu.stsi;
import zxfoundation.base.types;
import lib.bit;
import :common;

namespace arch::s390x::cpu::stsi::detail {

    /// @brief Issue STSI instruction to store system information.
    /// @param sysinfo 4KB-aligned output block.
    /// @param fc Function code (e.g. 1).
    /// @param sel1 Selector 1 (e.g. 1).
    /// @param sel2 Selector 2 (e.g. 1).
    /// @return 0 on success, -1 if not supported.
    auto invoke(void* sysinfo, u4 fc, u8 sel1, u8 sel2, u4& curr_level) noexcept -> std::expected<condition_code, lib::kernel_error> {
        u64 r0{0}, r1{0};
        lib::bit_insert_msb<u64>(r0, 32, 35, fc);
        lib::bit_insert_msb<u64>(r0, 56, 63, sel1);
        lib::bit_insert_msb<u64>(r1, 48, 63, sel2);

        condition_code cc{};
        register u64 reg0 __asm__("0") = r0;
        register u64 reg1 __asm__("1") = r1;

        __asm__ volatile (
            "   stsi    %[sysinfo]\n"
            "   ipm     %[cc]\n"
            "   srl     %[cc],28\n"
            : [cc] "=d" (cc), [sysinfo] "=Q" (*static_cast<char*>(sysinfo))
            : "d" (reg0), "d" (reg1)
            : "cc", "memory"
        );
        if (fc == 0)
            curr_level = lib::bit_extract_msb<u8>(reg0, 32, 35);
        if (cc == 3)
            return std::unexpected(lib::kernel_error::from_arch(lib::arch_error::code::stsi_failed));
        return cc;
    }

} // namespace arch::s390x::cpu::stsi::detail
