/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/cputimer.zcomponent/cputimer.cxx
/// @brief z/Architecture CPU Timer — raw hardware accessors (SPT / STPT).

module arch.s390x.cpu.cputimer;
import zxfoundation.base.types;

namespace arch::s390x::cpu::cputimer {

    /// @brief Set the CPU Timer to a decrement value (SPT instruction).
    /// @param val  64-bit decrement value.  The timer counts down in
    ///             TOD-clock units (4096 ticks/µs).  An external interrupt
    ///             fires when the value wraps from 1 to 0.
    auto spt(u64 val) noexcept -> void {
        __asm__ volatile ("spt %[v]\n\t" :: [v] "Q"(val) : "memory");
    }

    /// @brief Read the current CPU Timer value (STPT instruction).
    /// @return  Current 64-bit decrement counter.
    [[nodiscard]] auto stpt() noexcept -> u64 {
        u64 v = 0;
        __asm__ volatile ("stpt %[v]\n\t" : [v] "=Q"(v) :: "memory");
        return v;
    }

} // namespace arch::s390x::cpu::cputimer
