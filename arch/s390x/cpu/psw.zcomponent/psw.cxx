/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/psw.cxxm
/// @brief z/Architecture PSW operations.

module arch.s390x.cpu.psw;
import zxfoundation.base.types;
import lib.stl;

namespace arch::s390x::cpu::psw {

    /// @brief Extract the current PSW mask via EPSW. addr is unavailable; set to 0.
    [[nodiscard]] auto extract_psw() noexcept -> zpsw {
        epsw_words w{};
        __asm__ volatile ("epsw %[hi], %[lo]"
            : [hi] "=d"(w.hi), [lo] "=d"(w.lo) :: "cc");
        return zpsw{ "mask"_f = std::bit_cast<u64>(w), "address"_f = 0 };
    }

    [[noreturn]] auto load_psw(const zpsw& psw) noexcept -> void {
        __asm__ volatile ("lpswe %0" :: "Q"(psw) : "memory", "cc");
        std::unreachable();
    }

    [[noreturn]] auto disabled_wait() noexcept -> void {
        alignas(8) static constexpr zpsw halt_psw {
            "mask"_f = PSW_MASK_DISABLED_WAIT, "address"_f = 0
        };
        load_psw(halt_psw);
    }

    /// @brief Enter enabled-wait state — the z/Architecture idle primitive.
    auto enabled_wait() noexcept -> void {
        alignas(8) static constexpr zpsw wait_psw {
            "mask"_f = PSW_MASK_ENABLED_WAIT, "address"_f = 0
        };
        __asm__ volatile(
            "   larl  1,0f\n"          // r1 = address of label '0:'
            "   stg   1,8(%[psw])\n"   // store as PSW instruction address
            "   lpswe 0(%[psw])\n"     // enter enabled wait (atomic)
            "0:\n"                     // resume point after interrupt
            :
            : [psw] "a"(&wait_psw)
            : "memory", "cc", "1"
        );
    }

    auto write_psw_to_lowcore(u64 lc_offset, zpsw psw) noexcept -> void {
        __asm__ volatile (
            "stg %[mask], 0(%[off])\n\t"
            "stg %[addr], 8(%[off])\n\t"
            :: [mask] "r"(psw.get<"mask">().underlying()),
               [addr] "r"(psw.get<"address">()),
               [off] "a"(lc_offset) : "memory"
        );
    }

    [[nodiscard]] auto make_handler_psw(u64 entry_virt) noexcept -> zpsw {
        return zpsw {
            "mask"_f = PSW_MASK_KERNEL_DAT, "address"_f = entry_virt
        };
    }

    [[nodiscard]] auto make_ap_restart_psw(u64 entry_phys) noexcept -> zpsw {
        return zpsw {
            "mask"_f = PSW_MASK_KERNEL, "address"_f = entry_phys
        };
    }

} // namespace arch::s390x::cpu::psw
