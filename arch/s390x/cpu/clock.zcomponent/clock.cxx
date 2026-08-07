/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/clock.cxxm
/// @brief z/Architecture TOD clock instructions — STCK, STCKF, STCKE.

module arch.s390x.cpu.clock;
import zxfoundation.base.types;
import arch.s390x.cpu.features;

namespace arch::s390x::cpu::clock {

    /// @brief Store Clock (STCK) — stores the 64-bit TOD clock value.
    /// @return 64-bit TOD clock value.
    /// @note   STCK sets CC=1 if the clock is non-operational.  We ignore CC here;
    ///         callers that need error handling should use stckf().
    auto stck() noexcept -> u64 {
        u64 tod;
        __asm__ volatile ("stck %0" : "=Q" (tod) :: "cc", "memory");
        return tod;
    }

    /// @brief Store Clock Fast (STCKF) — stores the 64-bit TOD clock value.
    /// @return 64-bit TOD clock value.
    /// @note   Falls back to STCK when facility bit 34 (GEN_INST) is absent.
    ///         The check uses the kernel's cached facility list (features module).
    auto stckf() noexcept -> u64 {
        u64 tod{0};
        if (features::has(features::facility::gen_inst_ext)) [[likely]] {
            __asm__ volatile("stckf %0" : "=Q" (tod) :: "cc", "memory");
        } else {
            tod = stck();
        }
        return tod;
    }

    /// @brief Store Clock Extended (STCKE) — stores 128-bit TOD clock + epoch index.
    /// @return 128-bit extended TOD value.
    auto stcke() noexcept -> tod_clock {
        tod_clock clk{};
        __asm__ volatile ("stcke %0" : "=Q" (clk) :: "cc", "memory");
        return clk;
    }

    /// @brief Load a new value into the clock comparator register (SCKC).
    /// @param[in] time  Absolute TOD clock value at which to fire.
    auto sckc(u64 time) noexcept -> void {
        __asm__ volatile("sckc %0" : : "Q" (time));
    }

    /// @brief Read the current clock comparator value (STCKC).
    /// @return The 64-bit TOD value currently loaded in the comparator.
    [[nodiscard]] auto stckc() noexcept -> u64 {
        u64 v = 0;
        __asm__ volatile ("stckc %[v]\n\t" : [v] "=Q"(v) :: "memory");
        return v;
    }

    /// @brief Set Clock Programmable Field (SCKPF)
    auto sckpf(u16 val) noexcept -> void {
        __asm__ volatile(
        "	lgr	0,%[val]\n"
        "	sckpf"
        :
        : [val] "d" (static_cast<unsigned long>(val))
        : "0");
    }

} // namespace arch::s390x::cpu::clock
