/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/classed.zcomponent/core.cxx
/// @brief Strict-priority scheduler class dispatch surface.

module zxfoundation.sched.classed.core;
import zxfoundation.sched.policy;
import zxfoundation.sched.fair.core;
import zxfoundation.sched.idle;
import zxfoundation.sched.rt.core;
import zxfoundation.sched.deadline.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.base.typestate;
import lib.error;
import lib.stl;

namespace zxfoundation::sched::classed::detail {

    using zxfoundation::dgp::domain::domain;
    using zxfoundation::dgp::domain::domain_flags;
    using zxfoundation::sched::rq::run_queue;
    using zxfoundation::dgp::domain::strand;
    using zxfoundation::dgp::domain::strand_flags;
    using zxfoundation::dgp::domain::is_idle_strand;
    using zxfoundation::sched::sched_class_id;

    [[nodiscard]] auto owner_of(const strand& s) noexcept -> const domain* {
        return static_cast<const domain*>(s.owner);
    }

    [[nodiscard]] auto mutable_owner_of(strand& s) noexcept -> domain* {
        return static_cast<domain*>(s.owner);
    }

    [[nodiscard]] auto class_of(const strand& s) noexcept -> sched_class_id {
        if (is_idle_strand(s)) return sched_class_id::idle;
        if (s.sched_policy.class_id == sched_class_id::rt
            && s.sched_admission.class_id == sched_class_id::rt) return sched_class_id::rt;
        if (s.sched_policy.class_id == sched_class_id::deadline
            && s.sched_admission.class_id == sched_class_id::deadline) return sched_class_id::deadline;
        return sched_class_id::fair;
    }

} // namespace detail

namespace zxfoundation::sched::classed {

    using zxfoundation::dgp::domain::domain;
    using zxfoundation::dgp::domain::domain_flags;
    using zxfoundation::sched::rq::run_queue;
    using zxfoundation::dgp::domain::strand;

    /// @brief Return the effective class used for a currently existing strand.
    /// @param[in] s Strand whose owner and policy are inspected.
    /// @return Scheduler class currently assigned to the strand.
    [[nodiscard]] auto effective_class(const strand& s) noexcept -> sched_class_id {
        return detail::class_of(s);
    }

    /// @brief Validate a scheduler policy against the current class surface.
    /// @param[in] owner Domain requesting admission for one of its strands.
    /// @param[in] policy Requested scheduler policy.
    /// @return Admission result for accepted fair/idle policy, or a kernel error for unsupported classes.
    [[nodiscard]] auto validate_policy(
        const domain& owner,
        const sched_policy& policy
    ) noexcept -> std::expected<sched_admission_result, lib::kernel_error> {
        sched_admission_result result{};
        result.preferred_cpu = 0U;
        result.remote_node_allowed = true;
        result.smt_sibling_allowed = policy.allow_smt_sibling;
        result.class_id = policy.class_id;

        switch (policy.class_id) {
            case sched_class_id::idle:
                return std::unexpected(
                    lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
            case sched_class_id::fair:
                return result;
            case sched_class_id::rt:
                if (policy.rt_priority == 0U || policy.runtime_ns == 0U || policy.period_ns == 0U
                    || policy.runtime_ns > policy.period_ns) {
                    return std::unexpected(
                        lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                result.admitted_runtime_ns = policy.runtime_ns;
                result.admitted_period_ns = policy.period_ns;
                return result;
            case sched_class_id::deadline:
                if (policy.runtime_ns == 0U || policy.deadline_ns == 0U || policy.period_ns == 0U
                    || policy.runtime_ns > policy.deadline_ns || policy.deadline_ns > policy.period_ns) {
                    return std::unexpected(
                        lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                result.admitted_runtime_ns = policy.runtime_ns;
                result.admitted_period_ns = policy.period_ns;
                return result;
        }

        return std::unexpected(
            lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
    }

    /// @brief Admit a concrete strand into a scheduler policy before enqueue.
    /// @param[in,out] owner Domain that owns the strand and any class budget.
    /// @param[in,out] s Strand receiving scheduler admission metadata.
    /// @param[in] policy Requested policy.
    /// @return Admission record, or kernel_error when DGP authority or class limits deny the request.
    [[nodiscard]] auto admit_policy(
        domain& owner,
        strand& s,
        const sched_policy& policy
    ) noexcept -> std::expected<sched_admission_result, lib::kernel_error> {
        switch (policy.class_id) {
            case sched_class_id::idle:
            case sched_class_id::fair: {
                auto result = validate_policy(owner, policy);
                if (!result) return std::unexpected(result.error());
                s.sched_policy = policy;
                s.sched_admission = *result;
                return *result;
            }
            case sched_class_id::rt:
                return zxfoundation::sched::rt::admit(owner, s, policy);
            case sched_class_id::deadline:
                return zxfoundation::sched::deadline::admit(owner, s, policy);
        }

        return std::unexpected(
            lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
    }

    /// @brief Enqueue a strand through its scheduler class backend.
    /// @param[in,out] rq Run queue protected by the caller.
    /// @param[in,out] s Strand to insert into the selected class queue.
    /// @return Success when the class accepts the strand, or an error if the
    ///         class is unsupported or SMT isolation-sensitive placement
    ///         is violated on the target CPU.
    [[nodiscard]] auto enqueue(
        run_queue& rq,
        strand& s
    ) noexcept -> std::expected<void, lib::kernel_error> {
        switch (detail::class_of(s)) {
            case sched_class_id::deadline:
                return zxfoundation::sched::deadline::enqueue(rq, s);
            case sched_class_id::rt:
                return zxfoundation::sched::rt::enqueue(rq, s);
            case sched_class_id::fair:
                return zxfoundation::sched::fair::enqueue(rq, s);
            case sched_class_id::idle:
                return {};
        }

        return std::unexpected(
            lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
    }

    /// @brief Remove a strand through its scheduler class backend.
    /// @param[in,out] rq Run queue protected by the caller.
    /// @param[in,out] s Strand to remove from the selected class queue.
    auto dequeue(run_queue& rq, strand& s) noexcept -> void {
        switch (detail::class_of(s)) {
            case sched_class_id::deadline:
                zxfoundation::sched::deadline::dequeue(rq, s);
                return;
            case sched_class_id::idle:
                return;
            case sched_class_id::rt:
                zxfoundation::sched::rt::dequeue(rq, s);
                return;
            case sched_class_id::fair:
                zxfoundation::sched::fair::dequeue(rq, s);
                return;
        }
    }

    /// @brief Select the next runnable strand in strict scheduler class order.
    /// @param[in,out] rq Run queue protected by the caller.
    /// @return Selected strand, or the run queue idle strand when no runnable work exists.
    [[nodiscard]] auto select_next(run_queue& rq) noexcept -> strand* {
        if (auto* next = zxfoundation::sched::deadline::pick_next(rq)) return next;
        if (auto* next = zxfoundation::sched::rt::pick_next(rq)) return next;
        if (auto* next = zxfoundation::sched::fair::pick_next(rq)) return next;
        return zxfoundation::sched::idle::the_class.pick_next_strand(rq);
    }

    /// @brief Account one scheduler tick against the running strand's class.
    /// @param[in,out] rq Run queue protected by the caller.
    /// @param[in,out] s Currently running strand.
    auto tick(run_queue& rq, strand& s) noexcept -> void {
        switch (detail::class_of(s)) {
            case sched_class_id::deadline:
                zxfoundation::sched::deadline::tick(rq, s);
                return;
            case sched_class_id::idle:
                return;
            case sched_class_id::rt:
                zxfoundation::sched::rt::tick(rq, s);
                return;
            case sched_class_id::fair:
                zxfoundation::sched::fair::tick(rq, s);
                return;
        }
    }

    /// @brief Evaluate preemption through the waking strand's scheduler class.
    /// @param[in,out] rq Run queue protected by the caller.
    /// @param[in,out] waking Runnable strand that may preempt the current strand.
    auto check_preempt(run_queue& rq, strand& waking) noexcept -> void {
        switch (detail::class_of(waking)) {
            case sched_class_id::deadline:
                zxfoundation::sched::deadline::check_preempt(rq, waking);
                return;
            case sched_class_id::idle:
                return;
            case sched_class_id::rt:
                zxfoundation::sched::rt::check_preempt(rq, waking);
                return;
            case sched_class_id::fair:
                zxfoundation::sched::fair::preempt(rq, waking);
                return;
        }
    }

} // namespace zxfoundation::sched::classed