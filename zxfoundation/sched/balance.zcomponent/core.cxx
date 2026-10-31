/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/balance.zcomponent/core.cxx
/// @brief Topology-aware DGP scheduler balancing policy helpers.

module zxfoundation.sched.balance.core;
import zxfoundation.sched.policy;
import zxfoundation.sched.topology.types;
import zxfoundation.sched.topology.core;
import zxfoundation.sched.sd.types;
import zxfoundation.sched.sd.core;
import zxfoundation.sched.smt.core;
import zxfoundation.sched.rq_snapshot.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.isolation.types;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.base.typestate;
import zxfoundation.sched.fair.core;
import zxfoundation.sched.context.core;
import zxfoundation.sched.admit.core;
import zxfoundation.sched.core;
import zxfoundation.sched.rq.types;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.lowcore;
import lib.error;
import lib.rbtree;
import lib.stl;

namespace zxfoundation::sched::balance::detail {

    /// @brief Score-weighting constants — 1024 = 1.0 scaled fixed-point.
    /// @note Mirrors Linux's @c source_load / @c target_load weights;
    constexpr u32 BALANCE_MIN_SCORE    = 8U;    ///< below this, no migration
    constexpr u32 LOAD_SCALE_FACTOR    = 1024U;
    [[maybe_unused]] constexpr u64 LOAD_AVG_DIVISOR     = 1024U;  ///< PELT @c load_avg ceiling
    [[maybe_unused]] constexpr u32 CAPACITY_REFLAT       = 1024U;  ///< Max @c cpu_capacity_curr

    /// @brief Decide the lock-acquisition order for double-RQ locking.
    [[nodiscard]] inline auto lock_order(
        rq::run_queue& a,
        rq::run_queue& b
    ) noexcept -> std::pair<rq::run_queue*, rq::run_queue*> {
        if (a.cpu_id < b.cpu_id) return { &a, &b };
        return { &b, &a };
    }
    
} // namespace detail

namespace zxfoundation::sched::balance {

    /// @brief Build the scheduler placement policy for a strand from its owning DGP domain.
    /// @param[in] s Strand whose owner provides placement metadata.
    /// @return Placement policy describing preferred NUMA node and SMT isolation sensitivity.
    [[nodiscard]] auto placement_policy_for(
        const zxfoundation::dgp::domain::strand& s
    ) noexcept -> topology::strand_placement_policy {
        topology::strand_placement_policy policy{};
        const auto* owner = static_cast<const zxfoundation::dgp::domain::domain*>(s.owner);
        if (!owner) return policy;

        policy.preferred_node = owner->numa_node;
        policy.allow_remote_node = true;
        policy.allow_smt_sibling = !zxfoundation::dgp::has_all_rights(
            owner->granted_rights,
            ::zxfoundation::dgp::right::sched);
        policy.isolation_sensitive = !policy.allow_smt_sibling;
        return policy;
    }

    /// @brief Test whether a candidate CPU may run a strand under its placement policy.
    /// @param[in] s Strand being migrated or admitted.
    /// @param[in] target_cpu Candidate logical CPU.
    /// @return Success when the CPU is online and satisfies the DGP placement constraints.
    [[nodiscard]] auto validate_cpu_for_strand(
        const zxfoundation::dgp::domain::strand& s,
        u16 target_cpu
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const auto* target = topology::record_for(target_cpu);
        if (!target || target->state != topology::cpu_scheduling_state::online) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        const auto policy = placement_policy_for(s);
        if (policy.isolation_sensitive && !policy.allow_smt_sibling
            and target->smt_evidence == topology_evidence::degraded) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        if (!policy.allow_remote_node && policy.preferred_node != 0xFFU
            && target->numa_node != policy.preferred_node) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        return {};
    }

    /// @brief Score a source CPU as a steal candidate for a local CPU.
    /// @param[in] local_cpu  Logical CPU requesting work.
    /// @param[in] source_cpu  Logical CPU that may donate work.
    /// @param[in] source_load Atomic counter (PELT load_avg preferred)
    ///                        on the source CPU.
    /// @param[in] source_capacity CPU capacity divisor (1024 = full).
    /// @param[in] tier       Maximum allowed topology distance for this balancing pass.
    /// @return A non-zero score when the source is eligible; higher scores are preferred.
    [[nodiscard]] auto steal_source_score(
        u16 local_cpu,
        u16 source_cpu,
        u32 source_load,
        u32 source_capacity,
        topology::cpu_relation tier
    ) noexcept -> u32 {
        if (source_load == 0U) return 0U;

        const auto relation = topology::relation_between(local_cpu, source_cpu);
        if (relation == topology::cpu_relation::unavailable || relation == topology::cpu_relation::self) {
            return 0U;
        }

        if (static_cast<u8>(relation) > static_cast<u8>(tier)) {
            return 0U;
        }

        const u32 capacity = (source_capacity == 0U) ? 100U : source_capacity;
        const u32 distance_penalty = static_cast<u32>(relation) * 8U;
        const u64 load_score =
            static_cast<u64>(source_load) * 8U *
            (static_cast<u64>(capacity) + detail::LOAD_SCALE_FACTOR) /
            detail::LOAD_SCALE_FACTOR;
        if (load_score <= distance_penalty) {
            return 0U;
        }
        const u32 score = static_cast<u32>(load_score - distance_penalty);
        return (score >= detail::BALANCE_MIN_SCORE) ? score : 0U;
    }

    /// @brief Walk the @c sched_domain hierarchy and pull the busiest peer's
    ///        rightmost fair strand to the local CPU.
    /// @param[in] cpu  CPU id the softirq fired on (the pulling / "this" CPU).
    /// @param[in] kind Balance kind (periodic / nohz_idle / forced).  Currently
    ///                 @e any kind drives a pull attempt.
    /// @param[out] req Per-CPU balance request carrier — updated with the
    ///                 pass outcome ( @c last_migration_ok set to 1 on success).
    auto balance_dispatch(u16 cpu, softirq::balance_kind kind,
                           softirq::balance_req& req) noexcept -> void {
        discard_value kind;

        namespace cpumask = arch::s390x::cpu::cpumask;

        auto& local_rq = cpu_rq(cpu);
        if (!local_rq.online.load(std::memory_order::acquire)) return;

        if (rq_snapshot::steal_epoch_slot() != cpu) {
            req.last_migration_ok.store(0U, std::memory_order::release);
            return;
        }
        rq_snapshot::steal_epoch_advance(cpu);

        topology::refresh_cpu_record(cpu, local_rq.nr_running, true);

        constexpr u16 CONFIG = zxfoundation::base::config::CONFIG_ZX_MAX_CPUS;
        struct scored_cpu { u16 cpu{CONFIG}; u32 score{0U}; rq_snapshot::rq_snapshot snap{}; };
        scored_cpu best{};

        const auto* local_rec = topology::record_for(cpu);
        if (!local_rec) return;

        const auto score_with_snapshot = [&](
            u16 src_cpu,
            const rq_snapshot::rq_snapshot& snap,
            topology::cpu_relation tier
        ) noexcept -> void {
            u32 load_for_score = static_cast<u32>(snap.cfs_load_avg);
            if (load_for_score == 0U) load_for_score = snap.nr_running;
            if (load_for_score <= 1U) return;
            const u32 capacity = snap.cpu_capacity_curr != 0U
                ? snap.cpu_capacity_curr
                : static_cast<u32>(local_rec ? local_rec->capacity : 100U);
            if (snap.nr_running <= 1U && snap.cfs_load_avg == 0U) return;
            const u32 s = steal_source_score(
                cpu, src_cpu, load_for_score, capacity, tier);
            if (s > best.score) {
                best.score = s;
                best.cpu   = src_cpu;
                best.snap  = snap;
            }
        };

        // Walk sched_domain hierarchy — finest (SMT) first, then up.
        const auto& tree = local_rq.sd_tree;
        for (u32 lvl = 0; lvl < sd::SCHED_DOMAIN_MAX_LEVEL && tree.levels[lvl]; ++lvl) {
            const auto* domain = tree.levels[lvl];
            const auto tier = static_cast<topology::cpu_relation>(
                static_cast<u8>(topology::cpu_relation::same_numa_node)
                + static_cast<u8>(domain->level));
            sd::for_each_group(*domain, [&](const sd::sched_group& grp) noexcept -> void {
                grp.cpus.for_each([&](u16 gcpu) noexcept -> void {
                    if (gcpu == cpu) return;
                    if (!cpumask::cpu_online(gcpu)) return;
                    rq_snapshot::rq_snapshot snap{};
                    auto r = rq_snapshot::collect(gcpu, snap);
                    if (!r) return;
                    if (snap.online == 0U) return;
                    score_with_snapshot(gcpu, snap, tier);
                });
            });
            if (best.cpu < CONFIG) break;
        }

        if (best.cpu >= CONFIG || best.score < detail::BALANCE_MIN_SCORE) {
            req.last_migration_ok.store(0U, std::memory_order::release);
            return;
        }

        auto& src_rq = cpu_rq(best.cpu);
        const auto order = detail::lock_order(local_rq, src_rq);
        order.first->lock.lock();
        if (order.first != order.second) order.second->lock.lock();

        auto unlock_both = [&]() noexcept -> void {
            order.first->lock.unlock();
            if (order.second != order.first) order.second->lock.unlock();
        };

        if (!cpumask::cpu_online(src_rq.cpu_id) || src_rq.nr_running <= 1U) {
            unlock_both();
            req.last_migration_ok.store(0U, std::memory_order::release);
            return;
        }

        dgp::domain::strand* stolen = nullptr;
        auto* last = src_rq.fair_tree.root.node;
        if (last) {
            while (last->right) last = last->right;
            stolen = fair::rb_node_to_strand(last);
            if (stolen->cpu != src_rq.cpu_id || !stolen->on_rq) [[unlikely]] {
                unlock_both();
                req.last_migration_ok.store(0U, std::memory_order::release);
                return;
            }
            auto placement_result = validate_cpu_for_strand(*stolen, local_rq.cpu_id);
            const auto* stolen_domain = static_cast<const dgp::domain::domain*>(stolen->owner);
            if (stolen_domain
                && !zxfoundation::sched::admit::can_admit_domain(stolen_domain)) [[unlikely]] {
                placement_result = std::unexpected(
                    lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
            }

            if (placement_result && stolen_domain) {
                const auto wants_isolation = !zxfoundation::dgp::has_all_rights(
                    stolen_domain->granted_rights,
                    ::zxfoundation::dgp::right::sched);
                if (wants_isolation) {
                    const u32 src_dom   = stolen_domain->ko.id;
                    const u32 src_attach = best.snap.attached_domain_id;
                    if (src_attach != 0U && src_attach != src_dom) {
                        placement_result = std::unexpected(
                            lib::kernel_error::from_generic(
                                lib::generic_error::invalid_arg));
                    }
                    if (const auto* tgt_pcp =
                            arch::s390x::cpu::percpu::cpu_percpu(local_rq.cpu_id)) {
                        const u32 tgt_attach = tgt_pcp->attached.domain_id;
                        if (tgt_attach != 0U && tgt_attach != src_dom) {
                            placement_result = std::unexpected(
                                lib::kernel_error::from_generic(
                                    lib::generic_error::invalid_arg));
                        }
                    }
                }
            }
            if (!placement_result) {
                unlock_both();
                req.last_migration_ok.store(0U, std::memory_order::release);
                return;
            }

            const u64 src_min_vr = src_rq.min_vruntime;
            fair::dequeue(src_rq, *stolen);
            --src_rq.nr_running;

            fair::migrate_entity(stolen->se, src_min_vr, local_rq.min_vruntime);
            stolen->cpu = local_rq.cpu_id;

            auto enq_result = fair::enqueue(local_rq, *stolen);
            if (!enq_result) [[unlikely]] {
                stolen->cpu = src_rq.cpu_id;
                fair::migrate_entity(stolen->se, local_rq.min_vruntime, src_min_vr);
                discard_value fair::enqueue(src_rq, *stolen);
                rq_snapshot::publish(local_rq);
                rq_snapshot::publish(src_rq);
                unlock_both();
                req.last_migration_ok.store(0U, std::memory_order::release);
                return;
            }
            ++local_rq.nr_running;
        }

        rq_snapshot::publish(local_rq);
        rq_snapshot::publish(src_rq);

        unlock_both();

        topology::refresh_cpu_record(cpu, local_rq.nr_running,
                                       cpumask::cpu_online(cpu));

        if (local_rq.curr == local_rq.idle_strand) {
            auto* curr_dom = static_cast<dgp::domain::domain*>(
                arch::s390x::cpu::lowcore::current_domain_ptr());
            if (curr_dom) {
                curr_dom->flags = curr_dom->flags | dgp::domain::domain_flags::need_resched;
            }
        }

        req.last_migration_ok.store(
            (stolen != nullptr) ? 1U : 0U, std::memory_order::release);
    }

} // namespace zxfoundation::sched::balance