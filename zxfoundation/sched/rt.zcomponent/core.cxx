/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/rt.cxx
/// @brief Fixed-priority real-time scheduler backend with DGP admission gates.

module zxfoundation.sched.rt.core;
import zxfoundation.sched.policy;
import zxfoundation.sched.cpupri.core;
import zxfoundation.sched.rt.bandwidth;
import zxfoundation.sched.smt.core;
import zxfoundation.sched.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.isolation;
import zxfoundation.dgp.isolation.types;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.base.config;
import zxfoundation.sched.rq.types;
import arch.s390x.cpu.cpumask;
import lib.error;
import lib.list;
import lib.cpumask;
import std;

namespace zxfoundation::sched::rt::detail {

    using zxfoundation::dgp::domain::domain;
    using zxfoundation::dgp::domain::domain_flags;
    using zxfoundation::sched::rq::run_queue;
    using zxfoundation::dgp::domain::strand;
    using zxfoundation::sched::sched_class_id;

    [[nodiscard]] auto has_rt_authority(const domain& d) noexcept -> bool {
        if (d.rt_capability_token != 0U && d.rt_capability_token == d.admin_token) return true;
        return zxfoundation::dgp::isolation::has_all_rights(
            d.granted_rights,
            zxfoundation::dgp::isolation::isolation_rights::sched);
    }

    [[nodiscard]] auto policy_matches_domain_masks(
        const domain& d,
        const zxfoundation::sched::sched_policy& policy
    ) noexcept -> bool {
        if (d.allowed_cpus.bits != 0U && policy.allowed_cpus.bits != 0U
            && (policy.allowed_cpus.bits & ~d.allowed_cpus.bits) != 0U) {
            return false;
        }
        if (d.allowed_numa_nodes.bits != 0U && policy.allowed_numa_nodes.bits != 0U
            && (policy.allowed_numa_nodes.bits & ~d.allowed_numa_nodes.bits) != 0U) {
            return false;
        }
        return true;
    }

    [[nodiscard]] auto reserve_domain_runtime(
        domain& d,
        u64 runtime_ns,
        u64 period_ns
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (d.rt_period_ns == 0U) d.rt_period_ns = period_ns;
        if (d.rt_budget_ns == 0U) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        if (period_ns != d.rt_period_ns) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        if (runtime_ns > d.rt_budget_ns || d.rt_reserved_ns > d.rt_budget_ns - runtime_ns) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        d.rt_reserved_ns += runtime_ns;
        return {};
    }

    auto release_domain_runtime(domain& d, u64 runtime_ns) noexcept -> void {
        d.rt_reserved_ns = (d.rt_reserved_ns > runtime_ns) ? (d.rt_reserved_ns - runtime_ns) : 0U;
    }

} // namespace detail

namespace zxfoundation::sched::rt {

    using zxfoundation::dgp::domain::domain;
    using zxfoundation::sched::rq::run_queue;
    using zxfoundation::dgp::domain::strand;

    /// @brief Validate and reserve RT scheduling authority for a strand.
    /// @param[in,out] owner Domain that owns the strand and RT budget.
    /// @param[in,out] s Strand receiving RT admission metadata.
    /// @param[in] policy Requested RT scheduler policy.
    /// @return Admission result with reserved runtime and period, or kernel_error when denied.
    [[nodiscard]] auto admit(
        domain& owner,
        strand& s,
        const sched_policy& policy
    ) noexcept -> std::expected<sched_admission_result, lib::kernel_error> {
        if (!is_active_domain(owner) || is_idle_domain(owner)) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        if (policy.class_id != sched_class_id::rt || !detail::policy_matches_domain_masks(owner, policy)) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        auto dgp_result = zxfoundation::dgp::isolation::validate_scheduler_policy(
            owner.ko.state,
            policy,
            detail::has_rt_authority(owner));
        if (!dgp_result) {
            return std::unexpected(dgp_result.error());
        }

        auto reserve_result = detail::reserve_domain_runtime(
            owner,
            dgp_result->admitted_runtime_ns,
            dgp_result->admitted_period_ns);
        if (!reserve_result) {
            return std::unexpected(reserve_result.error());
        }

        s.sched_policy = policy;
        s.sched_admission = *dgp_result;
        s.body->rt_runtime_used_ns = 0U;
        s.body->rt_period_start_ns = 0U;
        return *dgp_result;
    }

    /// @brief Release RT bandwidth previously reserved for a strand.
    /// @param[in,out] owner Domain owning the reservation.
    /// @param[in,out] s Strand whose reservation is released.
    /// @note The function is idempotent for non-RT strands.
    auto release_admission(domain& owner, strand& s) noexcept -> void {
        if (s.sched_policy.class_id != sched_class_id::rt) return;
        detail::release_domain_runtime(owner, s.sched_admission.admitted_runtime_ns);
        s.sched_policy = sched_policy{};
        s.sched_admission = sched_admission_result{};
        s.body->rt_runtime_used_ns = 0U;
        s.body->rt_period_start_ns = 0U;
    }

    /// @brief Enqueue an admitted RT strand on a per-CPU run queue.
    /// @param[in,out] rq Run queue protected by the caller.
    /// @param[in,out] s Admitted real-time strand.
    /// @return Success or kernel_error if admission metadata is invalid or
    ///         SMT isolation-sensitive placement is violated on the target CPU.
    [[nodiscard]] auto enqueue(
        run_queue& rq,
        strand& s
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (s.sched_policy.class_id != sched_class_id::rt
            || s.sched_admission.class_id != sched_class_id::rt
            || s.sched_admission.admitted_runtime_ns == 0U) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        if (s.on_rq) return {};

        if (!s.sched_policy.allow_smt_sibling
            && zxfoundation::sched::smt::isolation_violation(s, rq.cpu_id)) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        const u8 old_top = cpupri::pri_top(rq.cpu_id);
        rq.rt_queue.push_back(s.sched_link);
        s.on_rq = true;
        ++rq.nr_running;

        const u8 new_top = s.sched_policy.rt_priority;
        cpupri::pri_set(rq.cpu_id, old_top, new_top);
        return {};
    }

    /// @brief Dequeue an admitted RT strand from a per-CPU run queue.
    /// @param[in,out] rq Run queue protected by the caller.
    /// @param[in,out] s Real-time strand to remove.
    auto dequeue(run_queue& rq, strand& s) noexcept -> void {
        if (!s.on_rq) return;

        const u8 old_top = cpupri::pri_top(rq.cpu_id);
        lib::list_head::remove(s.sched_link);
        s.on_rq = false;
        if (rq.nr_running) --rq.nr_running;

        u8 new_top = cpupri::CPUPRI_INVALID;
        strand* best = nullptr;
        for (auto& c : rq.rt_queue.iter<strand, &strand::sched_link>()) {
            if (!best || c.sched_policy.rt_priority > best->sched_policy.rt_priority)
                best = &c;
        }
        if (best) new_top = best->sched_policy.rt_priority;
        cpupri::pri_set(rq.cpu_id, old_top, new_top);
    }

    /// @brief Pick the highest-priority RT strand from a run queue.
    /// @param[in,out] rq Run queue protected by the caller.
    /// @return Best RT strand, or nullptr when no RT work is queued.
    [[nodiscard]] auto pick_next(run_queue& rq) noexcept -> strand* {
        strand* best = nullptr;
        for (auto& candidate : rq.rt_queue.iter<strand, &strand::sched_link>()) {
            if (!best || candidate.sched_policy.rt_priority > best->sched_policy.rt_priority) best = &candidate;
        }
        return best;
    }

    /// @brief Account an RT scheduler tick, charge per-CPU RT bandwidth, and
    ///        dequeue the strand on budget exhaustion or per-CPU throttle.
    /// @param[in,out] rq Run queue protected by the caller.
    /// @param[in,out] s  Currently running real-time strand.
    auto tick(run_queue& rq, strand& s) noexcept -> void {
        constexpr u64 RT_ACCOUNTING_TICK_NS = 1'000'000ULL;
        if (s.sched_policy.class_id != sched_class_id::rt) return;
        s.body->rt_runtime_used_ns += RT_ACCOUNTING_TICK_NS;

        discard_value rt::bw::bw_charge(rq.rt_bw, RT_ACCOUNTING_TICK_NS);

        if (rq.rt_bw.rt_throttled) {
            auto* owner = static_cast<domain*>(s.owner);
            if (owner) owner->flags |= zxfoundation::dgp::domain::domain_flags::need_resched;
            dequeue(rq, s);
            return;
        }

        if (s.body->rt_runtime_used_ns >= s.sched_admission.admitted_runtime_ns) {
            auto* owner = static_cast<domain*>(s.owner);
            if (owner) owner->flags |= zxfoundation::dgp::domain::domain_flags::need_resched;
            dequeue(rq, s);
        }
    }

    /// @brief Mark the current domain for preemption when RT work wakes.
    /// @param[in,out] rq Run queue protected by the caller.
    /// @param[in,out] waking Real-time strand being enqueued or woken.
    auto check_preempt(run_queue& rq, strand& waking) noexcept -> void {
        if (!rq.curr || waking.sched_policy.class_id != sched_class_id::rt) return;
        auto* curr_owner = static_cast<domain*>(rq.curr->owner);
        if (curr_owner) curr_owner->flags |= zxfoundation::dgp::domain::domain_flags::need_resched;
    }

    /// @brief Push the highest-priority RT strand from @c src_rq to a less
    ///        busy CPU (Phase 4.6 push/pull migration).
    /// @param[in,out] src_rq Source CPU run queue (lock held).
    /// @return Destination CPU id, or CONFIG_ZX_MAX_CPUS if no destination found.
    [[nodiscard]] auto push(run_queue& src_rq) noexcept -> u16 {
        strand* highest = nullptr;
        for (auto& c : src_rq.rt_queue.iter<strand, &strand::sched_link>()) {
            if (!highest || c.sched_policy.rt_priority > highest->sched_policy.rt_priority)
                highest = &c;
        }
        if (!highest) return static_cast<u16>(zxfoundation::base::config::CONFIG_ZX_MAX_CPUS);

        // Convert sched_mask (a single u64) to lib::cpumask (768-bit bitmap).
        ::lib::cpumask affinity{};
        if (highest->sched_policy.allowed_cpus.bits == 0U) {
            for (u32 i = 0; i < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++i)
                affinity.set(i, std::memory_order::relaxed);
        } else {
            u64 bits = highest->sched_policy.allowed_cpus.bits;
            for (u32 i = 0; i < 64U && bits; ++i, bits >>= 1) {
                if (bits & 1U) affinity.set(i, std::memory_order::relaxed);
            }
        }

        const u16 dest = cpupri::find_fitness(highest->sched_policy.rt_priority, affinity);
        if (dest >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) return dest;
        return dest;
    }

    /// @brief Pull the highest idle-RT candidate from another CPU (Phase 4.6).
    /// @param[in]     idle_cpu CPU performing the pull (lock held on its own RQ).
    /// @param[in,out] src_rq   Source CPU run queue (lock held; caller may derive
    ///                         source cpu_id from @c src_rq.cpu_id).
    /// @return Stolen strand, or nullptr.
    [[nodiscard]] auto pull(u16 idle_cpu, run_queue& src_rq) noexcept -> strand* {
        strand* highest = nullptr;
        for (auto& c : src_rq.rt_queue.iter<strand, &strand::sched_link>()) {
            if (!highest || c.sched_policy.rt_priority > highest->sched_policy.rt_priority)
                highest = &c;
        }
        if (!highest) return nullptr;

        const u64 aff_bits = highest->sched_policy.allowed_cpus.bits;
        if (aff_bits != 0U && idle_cpu < 64U && !((aff_bits >> idle_cpu) & 1U))
            return nullptr;
        dequeue(src_rq, *highest);
        highest->cpu = idle_cpu;
        highest->on_rq = false;
        return highest;
    }

    /// @brief Apply a priority inheritance boost to @c s.
    auto boost_prio(strand& s, u8 new_prio) noexcept -> void {
        if (!s.body) [[unlikely]] return;
        if (s.body->boosted_prio == 0U) {
            s.body->saved_class = s.sched_policy.class_id;
            s.body->saved_prio   = s.sched_policy.rt_priority;
        }
        if (new_prio > s.body->boosted_prio) {
            s.body->boosted_prio = new_prio;
            s.sched_policy.class_id    = sched_class_id::rt;
            s.sched_policy.rt_priority = new_prio;
        }
    }

    /// @brief Remove the priority boost on @c s and restore saved policy.
    /// @note  Called from @c rt_mutex::unlock after ownership handoff.
    auto unboost_prio(strand& s) noexcept -> void {
        if (!s.body) [[unlikely]] return;
        if (s.body->boosted_prio == 0U) return;
        s.body->boosted_prio = 0U;
        s.sched_policy.class_id    = s.body->saved_class;
        s.sched_policy.rt_priority = s.body->saved_prio;
    }

    /// @brief Wake a strand that was sleeping on an rt_mutex.
    auto task_wake_pi(strand& s) noexcept -> void {
        s.substate = sched_substate::runnable;
        enqueue_strand(&s);
        if (auto* dom = static_cast<domain*>(s.owner)) {
            dom->flags |= zxfoundation::dgp::domain::domain_flags::need_resched;
        }
    }

} // namespace zxfoundation::sched::rt