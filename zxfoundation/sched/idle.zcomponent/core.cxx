/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/idle.zcomponent/core.cxx
/// @brief Idle scheduling class and idle strand initialization.

module zxfoundation.sched.idle.core;
import zxfoundation.sched.classed.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.domain.core;
import zxfoundation.base.types;
import zxfoundation.sched.rq.types;

namespace zxfoundation::sched::idle::detail {

    using namespace zxfoundation::dgp::domain;

    auto idle_enqueue(rq::run_queue&, strand&) noexcept -> void {}
    auto idle_dequeue(rq::run_queue&, strand&) noexcept -> void {}
    auto idle_pick_next(rq::run_queue& rq) noexcept -> strand* { return rq.idle_strand; }
    auto idle_tick(rq::run_queue&, strand&) noexcept -> void {}
    auto idle_preempt(rq::run_queue&, strand&) noexcept -> void {}

} // namespace detail

namespace zxfoundation::sched::idle {

    const classed::sched_class the_class{
        detail::idle_enqueue,
        detail::idle_dequeue,
        detail::idle_pick_next,
        detail::idle_tick,
        detail::idle_preempt
    };
    
    /// @brief Initialize the per-CPU idle strand.
    /// @param cpu_id  Logical CPU this idle strand serves.
    /// @return Pointer to the idle strand, or nullptr on allocation failure.
    [[nodiscard]] auto init_idle_strand(u16 cpu_id) noexcept -> dgp::domain::strand* {
        auto result = zxfoundation::dgp::domain::idle_strand_create(cpu_id);
        if (!result) return nullptr;
        return *result;
    }

    /// @brief Destroy a per-CPU idle strand, releasing its SCOMS slot and stack.
    /// @param s Idle strand to destroy (must not be on_rq or running).
    /// @return True on success, false on error.
    [[nodiscard]] auto deinit_idle_strand(dgp::domain::strand& s) noexcept -> bool {
        auto result = zxfoundation::dgp::domain::idle_strand_destroy(s);
        return static_cast<bool>(result);
    }

} // namespace zxfoundation::sched::idle