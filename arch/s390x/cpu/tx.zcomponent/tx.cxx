/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/tx.zcomponent/tx.cxx
/// @brief Verified z/Architecture transactional execution primitives.

module arch.s390x.cpu.tx;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.features;
import lib.stl;

namespace arch::s390x::cpu::tx {

    /// @brief Check that TX is installed and enabled for this CPU.
    [[nodiscard]] auto available() noexcept -> bool {
        if (!features::has(features::facility::transactional_exec)) return false;
        ctlreg::cr0 cr0{0};
        ctlreg::crread<0, 0>(cr0);
        return cr0.get<"transactional_exec_control">();
    }

    /// @brief Check that constrained TX is installed and TX is enabled.
    [[nodiscard]] auto constrained_available() noexcept -> bool {
        return available() && features::has(features::facility::constr_te);
    }

} // namespace arch::s390x::cpu::tx
