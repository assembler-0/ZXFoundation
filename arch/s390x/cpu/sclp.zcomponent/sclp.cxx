/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/sclp.cxxm
/// @brief SCLP (Service Call Logical Processor) kernel API.

module arch.s390x.cpu.sclp;
import zxfoundation.base.types;

namespace arch::s390x::cpu::sclp {

    /// @brief Read SCLP configuration info.
    /// @param info Output SCCB.
    /// @return 0 on success.
    auto read_scp_info(read_info_sccb& info) noexcept -> condition_code {
        info.header.length = sizeof(read_info_sccb);
        info.header.function_code = 0x01;
        const condition_code cc = invoke<sclp_cmdw::read_scp_info>(&info);
        if (cc != 0)
            return cc;
        return (info.header.response_code == SCLP_RC_NORMAL_COMPLETION) ? 0 : -1;
    }

    /// @brief Read SCLP CPU info.
    /// @param info Output SCCB.
    /// @return 0 on success.
    auto read_cpu_info(read_cpu_info_sccb& info) noexcept -> condition_code {
        info.header.length = sizeof(read_cpu_info_sccb);
        info.header.function_code = 0x01;
        const condition_code cc = invoke<sclp_cmdw::read_cpu_info>(&info);
        if (cc != 0)
            return cc;
        return (info.header.response_code == SCLP_RC_NORMAL_COMPLETION) ? 0 : -1;
    }

} // namespace arch::s390x::cpu::sclp
