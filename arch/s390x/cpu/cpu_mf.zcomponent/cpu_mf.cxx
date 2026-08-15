/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/cpu_mf.cxxm
/// @brief CPU Measurement Facility kernel API.

module arch.s390x.cpu.cpu_mf;
import zxfoundation.base.types;

namespace arch::s390x::cpu::cpu_mf {

    /// @brief Query Counter Information (QCTRI).
    /// @param info Output info block.
    /// @return 0 on success.
    auto qctri(cpumf_ctr_info& info) noexcept -> i32 {
        i32 rc = -1;
        __asm__ volatile(
            "0: qctri  %[info]\n"
            "1: lhi    %[rc],0\n"
            "2:\n"
            : [rc] "+d" (rc), [info] "=Q" (info)
            :
            : "cc", "memory"
        );
        return rc;
    }

    /// @brief Query Sampling Information (QSI).
    /// @param info Output info block.
    /// @return 0 on success.
    auto qsi(qsi_param_block& info) noexcept -> i32 {
        i32 rc = -1;
        __asm__ volatile(
            "0: qsi    %[info]\n"
            "1: lhi    %[rc],0\n"
            "2:\n"
            : [rc] "+d" (rc), [info] "=Q" (info)
            :
            : "cc", "memory"
        );
        return rc;
    }

    /// @brief Load CPU Counter-Set Controls (LCCTL).
    /// @param ctl Control value.
    /// @return Condition code (0 = success).
    auto lcctl(u64 ctl) noexcept -> condition_code {
        condition_code cc;
        __asm__ volatile(
            "    lcctl  %[ctl]\n"
            "    ipm    %[cc]\n"
            "    srl    %[cc],28\n"
            : [cc] "=d" (cc)
            : [ctl] "Q" (ctl)
            : "cc", "memory"
        );
        return cc;
    }

    /// @brief Extract CPU Counter (ECCTR).
    /// @param ctr Counter number.
    /// @param val Output value.
    /// @return Condition code (0 = success).
    auto ecctr(u64 ctr, u64& val) noexcept -> condition_code {
        condition_code cc;
        u64 out;
        __asm__ volatile(
            "    ecctr  %[out],%[ctr]\n"
            "    ipm    %[cc]\n"
            "    srl    %[cc],28\n"
            : [cc] "=d" (cc), [out] "=d" (out)
            : [ctr] "d" (ctr)
            : "cc", "memory"
        );
        if (cc == 0)
            val = out;
        return cc;
    }

    /// @brief Load Sampling Controls (LSCTL).
    /// @param req LSCTL Parameter block.
    /// @return Condition code.
    auto lsctl(lsctl_param_block& req) -> condition_code {
        condition_code cc;
        int exception = 1;
        __asm__ volatile(
            "0:	lsctl	%[req]\n"
            "1:	lhi	%[exc],0\n"
            "2:\n"
            "ipm	%[cc]\n"
            "srl  %[cc],28\n"
            : [cc] "=d" (cc), [exc] "+d" (exception)
            : [req] "Q" (req)
            : "cc");
        if (exception || cc)
            return -1;
        return 0;
    }


} // namespace arch::s390x::cpu::cpu_mf
