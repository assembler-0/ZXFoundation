/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/admit.cxxm
/// @brief DGP scheduler admission gate.

module zxfoundation.sched.admit.core;
import zxfoundation.base.types;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.isolation;
import zxfoundation.dgp.isolation.types;

namespace zxfoundation::sched::admit {

    /// @brief DGP scheduler admission gate.
    /// @param[in] d Domain attempting to enqueue a strand on any CPU.
    /// @return True when @c d may run a strand; false otherwise.
    [[nodiscard]] auto can_admit_domain(
        const zxfoundation::dgp::domain::domain* d
    ) noexcept -> bool {
        if (!d) return true;
        if (zxfoundation::dgp::domain::is_idle_domain(*d)) return true;
        auto result = zxfoundation::dgp::isolation::validate_scheduler_admission(
            d->ko.state, d->root_phys, d->asce.val);
        return static_cast<bool>(result);
    }

} // namespace zxfoundation::sched::admit
