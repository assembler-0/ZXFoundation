/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/core.cxx
/// @brief Scheduler core — schedule(), tick, init, work-stealing, softirq, nohz.

module zxfoundation.sched.core;
import zxfoundation.sched.classed.core;
import zxfoundation.sched.policy;
import zxfoundation.sched.entity;
import zxfoundation.sched.fair.core;
import zxfoundation.sched.topology.types;
import zxfoundation.sched.topology.core;
import zxfoundation.sched.balance.core;
import zxfoundation.sched.rq_snapshot.core;
import zxfoundation.sched.idle;
import zxfoundation.sched.strandctx.core;
import zxfoundation.sched.sd.types;
import zxfoundation.sched.sd.core;
import zxfoundation.sched.softirq.core;
import zxfoundation.sched.nohz.core;
import zxfoundation.sched.numa.core;
import zxfoundation.sched.pelt.core;
import zxfoundation.sched.cpupri.core;
import zxfoundation.sched.cpudl.core;
import zxfoundation.sched.rt.bandwidth;
import zxfoundation.sched.cbs.core;
import zxfoundation.sched.smt.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.domain.core;
import zxfoundation.dgp.signal;
import zxfoundation.dgp.gate.core;
import zxfoundation.dgp.portal;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.base.config;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.dgp.isolation;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.sync_contract;
import zxfoundation.memory.hhdm;
import zxfoundation.memory.numa.types;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.timer.types;
import zxfoundation.sys.timer.core;
import zxfoundation.sys.time.core;
import zxfoundation.sys.irq.core;
import zxfoundation.sys.irq.consts;
import zxfoundation.sched.context.core;
import zxfoundation.sched.admit.core;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.rcu.core;
import zxfoundation.sched.rt.core;
import zxfoundation.smp.csd.core;
import zxfoundation.sys.zinit.core;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.aste;
import arch.s390x.cpu.pcss.core;
import arch.s390x.cpu.ar;
import arch.s390x.cpu.clock;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.ipi;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.cputimer;
import drivers.watchdog.core;
import lib.error;
import lib.format;
import lib.rbtree;
import lib.list;
import lib.softirq.core;
import lib.cpumask;
import lib.container_of;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace zxfoundation::sched::detail {

    using namespace zxfoundation::dgp::domain;
    using namespace zxfoundation::sys::printk;

    static std::array<run_queue, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_rqs{};
    static bool g_sched_initialized{false};
    static std::array<u64, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_cputimer_armed_tod{};
    static sys::timer::kernel_timer g_bsp_tick_timer{};
    static std::array<sys::timer::kernel_timer, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_ap_tick_timers{};

    [[nodiscard]] auto pcp_cpu_id() noexcept -> u16 {
        return arch::s390x::cpu::percpu::this_cpu().cpu_id;
    }

    /// Arm SPT with @p ns (nanoseconds) and remember the armed value.
    auto cputimer_arm_ns(u64 ns) noexcept -> void {
        const u64 tod = (ns << 9U) / 125ULL;
        arch::s390x::cpu::cputimer::spt(tod);
        detail::g_cputimer_armed_tod[pcp_cpu_id()] = tod;
    }

    /// @brief Charge @p s the real runtime elapsed since its dispatch stamp
    auto runtime_charge(run_queue& rq, strand& s, u64 now_ns) noexcept -> void {
        if (!s.body) return;
        const u64 stamp = s.body->sched_stamp_ns;
        s.body->sched_stamp_ns = now_ns;
        if (stamp == 0ULL || stamp >= now_ns) return;
        zxfoundation::sched::fair::update_curr(
            s.se, rq.min_vruntime, now_ns - stamp);
    }

    auto validate_scheduler_irq_context(u32 domain_id = 0U) noexcept -> void {
        const auto state = zxfoundation::dgp::sync_contract::contract_snapshot(
            zxfoundation::dgp::sync_contract::lock_context::irq_disabled,
            zxfoundation::dgp::sync_contract::teardown_phase::none,
            domain_id);
        auto result = zxfoundation::dgp::sync_contract::validate_irq_state(
            state,
            zxfoundation::dgp::sync_contract::irq_contract::must_be_disabled);
        if (!result) [[unlikely]] {
            zxfoundation::sys::syschk::syschk_fatal(
                result.error(),
                "sched: DGP synchronization contract violated");
        }
    }

    auto domain_reap(domain& d) noexcept -> void {
        if (d.ko.id == zxfoundation::scoms::kobject::KOBJECT_ID_INVALID) return;
        if (d.ko.state != zxfoundation::base::lifecycle_state::dying) return;

        for (u32 i = 0; i < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++i) {
            const strand* rq_curr = detail::g_rqs[i].curr;
            if (rq_curr && static_cast<const domain*>(rq_curr->owner) == &d) {
                arch::s390x::cpu::ipi::send_reschedule(static_cast<u16>(i));
                return;
            }
        }

        discard_value zxfoundation::dgp::domain::domain_teardown(&d);
    }

    auto record_asce_attachment(domain& d) noexcept -> void {
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        pcp.attached.publish(d.asce.val, reinterpret_cast<u64>(&d), d.ko.id,
            static_cast<u8>(d.ko.state));
        auto& rq = detail::g_rqs[pcp.cpu_id];
        rq.balance_attached_asce.store(pcp.attached.asce, std::memory_order::release);
        rq.balance_attached_domain_id.store(pcp.attached.domain_id, std::memory_order::release);
        rq.balance_attached_dgp_state.store(pcp.attached.dgp_state, std::memory_order::release);
    }

    /// @brief Hierarchical work stealing walking the @c sched_domain tree.
    /// @param[in,out] local_rq  The idle CPU's run queue (lock held on entry).
    /// @return A stolen strand, or nullptr.
    /// @note The stolen strand's @c se.vruntime is normalized to the local
    ///       run queue's min_vruntime;
    auto try_steal_strand_locked(run_queue& local_rq) noexcept -> strand* {
        namespace fair = zxfoundation::sched::fair;
        namespace balance = zxfoundation::sched::balance;
        namespace topology = zxfoundation::sched::topology;
        namespace sd = zxfoundation::sched::sd;
        namespace cpumask = arch::s390x::cpu::cpumask;

        topology::refresh_cpu_record(local_rq.cpu_id, local_rq.nr_running,
                                      cpumask::cpu_online(local_rq.cpu_id));

        run_queue* busiest = nullptr;
        u32 best_score = 0U;

        const auto* dom_root = local_rq.sd_tree.levels[0];

        // Fallback to cpu_relation tiers when the domain tree is not yet warm.
        if (!dom_root) {
            constexpr topology::cpu_relation tiers[] {
                topology::cpu_relation::same_smt_core,
                topology::cpu_relation::same_core_group,
                topology::cpu_relation::same_numa_node,
                topology::cpu_relation::remote_numa_node,
            };
            for (const auto tier : tiers) {
                topology::for_each_steal_candidate(
                    local_rq.cpu_id, tier,
                    [&](u16 i) noexcept -> void {
                        auto& rq = g_rqs[i];
                        if (&rq == &local_rq) return;
                        const bool online = cpumask::cpu_online(rq.cpu_id);
                        if (!online) return;
                        zxfoundation::sched::rq_snapshot::rq_snapshot snap{};
                        if (!zxfoundation::sched::rq_snapshot::collect(rq.cpu_id, snap)) return;
                        const u32 score = balance::steal_source_score(
                            local_rq.cpu_id, rq.cpu_id, snap.nr_running,
                            snap.cpu_capacity_curr, tier);
                        if (score > best_score) { best_score = score; busiest = &rq; }
                    });
                if (busiest) break;
            }
        } else {
            for (const sd::sched_domain* sd = dom_root; sd != nullptr; sd = sd->parent) {
                sd::for_each_group(*sd, [&](const sd::sched_group& sg) noexcept -> void {
                    sg.cpus.for_each([&](u16 i) noexcept -> void {
                        auto& rq = g_rqs[i];
                        if (&rq == &local_rq) return;
                        const bool online = cpumask::cpu_online(rq.cpu_id);
                        if (!online) return;
                        zxfoundation::sched::rq_snapshot::rq_snapshot snap{};
                        if (!zxfoundation::sched::rq_snapshot::collect(rq.cpu_id, snap)) return;
                        if (snap.nr_running <= 1U) return;
                        const auto tier = static_cast<topology::cpu_relation>(
                            static_cast<u8>(topology::cpu_relation::same_numa_node)
                            + static_cast<u8>(sd->level));
                        const u32 score = balance::steal_source_score(
                            local_rq.cpu_id, rq.cpu_id, snap.nr_running,
                            snap.cpu_capacity_curr, tier);
                        if (score > best_score) { best_score = score; busiest = &rq; }
                    });
                });
                if (busiest) break;
            }
        }

        if (!busiest) return nullptr;

        run_queue& first  = (local_rq.cpu_id < busiest->cpu_id) ? local_rq : *busiest;
        run_queue& second = (local_rq.cpu_id < busiest->cpu_id) ? *busiest : local_rq;
        if (&first != &local_rq) first.lock.lock();
        if (&second != &local_rq) second.lock.lock();

        if (!cpumask::cpu_online(busiest->cpu_id) || busiest->nr_running <= 1) {
            if (&second != &local_rq) second.lock.unlock();
            if (&first != &local_rq) first.lock.unlock();
            return nullptr;
        }

        strand* stolen = nullptr;
        auto* last = busiest->fair_tree.root.node;
        if (last) {
            while (last->right) last = last->right;
            stolen = fair::rb_node_to_strand(last);
            const auto* stolen_domain = static_cast<const domain*>(stolen->owner);
            auto placement_result = balance::validate_cpu_for_strand(*stolen, local_rq.cpu_id);
            if (stolen->cpu != busiest->cpu_id || !stolen->on_rq
                || !zxfoundation::sched::admit::can_admit_domain(stolen_domain) || !placement_result) [[unlikely]] {
                stolen = nullptr;
            } else {
                const u64 source_min_vruntime = busiest->min_vruntime;
                fair::dequeue(*busiest, *stolen);
                stolen->cpu = local_rq.cpu_id;
                zxfoundation::sched::fair::migrate_entity(stolen->se, source_min_vruntime,
                                                           local_rq.min_vruntime);
            }
        }

        zxfoundation::sched::rq_snapshot::publish(local_rq);
        zxfoundation::sched::rq_snapshot::publish(*busiest);
        if (&second != &local_rq) second.lock.unlock();
        if (&first != &local_rq) first.lock.unlock();
        return stolen;
    }

    auto set_need_resched() noexcept -> void {
        auto* dom = static_cast<domain*>(
            arch::s390x::cpu::lowcore::current_domain_ptr());
        if (!dom) return;
        dom->flags = dom->flags | domain_flags::need_resched;
    }

} // namespace detail

namespace zxfoundation::sched {

    using zxfoundation::dgp::domain::domain;
    using zxfoundation::dgp::domain::domain_id;
    using zxfoundation::dgp::domain::domain_flags;
    using zxfoundation::dgp::domain::strand;
    using zxfoundation::sched::sched_substate;
    using zxfoundation::base::lifecycle_state;
    using zxfoundation::sched::rq::run_queue;
    using zxfoundation::sched::sched_entity;
    using zxfoundation::sched::sd::sched_domain_tree;

    using strandctx::setup_primary_strand_context;
    using strandctx::setup_strand_context_kernel_thread;
    using strandctx::setup_strand_context_user;
    using strandctx::setup_primary_strand_context_user;
    using strandctx::domain_enter_user;

    /// @brief Return the per-CPU periodic tick timer used by the nohz machinery.
    [[nodiscard]] auto cpu_timer(u16 cpu_id) noexcept -> zxfoundation::sys::timer::kernel_timer& {
        return (cpu_id == 0U) ? detail::g_bsp_tick_timer : detail::g_ap_tick_timers[cpu_id];
    }

    [[nodiscard]] auto cpu_rq(u16 cpu_id) noexcept -> run_queue& {
        return detail::g_rqs[cpu_id];
    }

    [[nodiscard]] auto this_rq() noexcept -> run_queue& {
        const auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        return detail::g_rqs[pcp.cpu_id];
    }

    auto enqueue_strand(strand* s) noexcept -> void {
        auto* d = static_cast<domain*>(s->owner);
        if (d
            && (d->ko.state == zxfoundation::base::lifecycle_state::dying
                || d->ko.state == zxfoundation::base::lifecycle_state::destroyed)) {
            return;
        }
        if ((s->flags & zxfoundation::dgp::domain::strand_flags::dead)
            != zxfoundation::dgp::domain::strand_flags::none) {
            return;
        }
        auto& rq = cpu_rq(s->cpu);
        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        detail::validate_scheduler_irq_context(d ? d->ko.id : 0U);
        rq.lock.lock();
        auto enqueue_result = classed::enqueue(rq, *s);
        if (!enqueue_result) [[unlikely]] {
            rq.lock.unlock();
            arch::s390x::cpu::irq::irq_restore(flags);
            zxfoundation::sys::syschk::syschk_fatal(
                enqueue_result.error(),
                "sched: scheduler class enqueue denied");
        }
        rq_snapshot::publish(rq);
        rq.lock.unlock();
        arch::s390x::cpu::irq::irq_restore(flags);

        const auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        if (s->cpu != pcp.cpu_id) {
            arch::s390x::cpu::ipi::send_reschedule(s->cpu);
        } else {
            if (rq.curr == rq.idle_strand) {
                auto* curr_dom = static_cast<domain*>(
                    arch::s390x::cpu::lowcore::current_domain_ptr());
                if (curr_dom) {
                    curr_dom->flags = curr_dom->flags | domain_flags::need_resched;
                }
            }
        }
        softirq::raise_balance_local(softirq::balance_kind::periodic);
    }

    /// @brief Wake a parked strand through the canonical wake discipline.
    /// @param[in,out] s Strand to wake.
    auto wake_strand(strand& s) noexcept -> void {
        auto* owner = static_cast<domain*>(s.owner);
        if ((s.flags & zxfoundation::dgp::domain::strand_flags::dead)
            != zxfoundation::dgp::domain::strand_flags::none) {
            return;
        }
        if (s.substate != sched_substate::blocked) {
            if (owner) {
                owner->flags = owner->flags | domain_flags::need_resched;
            }
            return;
        }
        s.substate = sched_substate::runnable;
        enqueue_strand(&s);
        if (owner) {
            owner->flags = owner->flags | domain_flags::need_resched;
        }
    }

    auto dequeue_strand(strand* s) noexcept -> void {
        auto& rq = cpu_rq(s->cpu);
        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        detail::validate_scheduler_irq_context();
        rq.lock.lock();
        classed::dequeue(rq, *s);
        rq_snapshot::publish(rq);
        rq.lock.unlock();
        arch::s390x::cpu::irq::irq_restore(flags);
    }

    auto enqueue_domain(domain* d) noexcept -> void {
        enqueue_strand(&d->primary_strand);
    }

    /// @brief Admit a scheduler policy for a strand before run-queue insertion.
    /// @param[in,out] s Strand receiving the policy metadata.
    /// @param[in] policy Requested scheduler policy.
    /// @return Admission record on success, or kernel_error when DGP/class policy denies the request.
    [[nodiscard]] auto strand_admit_policy(
        strand& s,
        const zxfoundation::sched::sched_policy& policy
    ) noexcept -> std::expected<zxfoundation::sched::sched_admission_result, lib::kernel_error> {
        auto* owner = static_cast<domain*>(s.owner);
        if (!owner || s.on_rq) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        return classed::admit_policy(*owner, s, policy);
    }

    /// @brief Create and optionally enqueue a secondary strand for a DGP domain.
    /// @param[in] request Executor-style creation request
    /// @return Pointer to the created strand, or @c kernel_error
    [[nodiscard]] auto strand_spawn(
        const zxfoundation::dgp::domain::strand_create_request& request
    ) noexcept -> std::expected<zxfoundation::dgp::domain::strand*, lib::kernel_error> {
        const bool kthread_mode = (request.entry != nullptr);
        const bool user_mode    = (request.uregs != nullptr);

        if (kthread_mode == user_mode) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        if (user_mode && request.uregs->psw_addr == 0U) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        auto* owner = zxfoundation::dgp::domain::domain_find(request.owner_domain);
        if (!owner) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));
        }

        auto create_result = zxfoundation::dgp::domain::strand_create(owner, request.cpu);
        if (!create_result) {
            return std::unexpected(create_result.error());
        }

        auto* s = *create_result;
        if (kthread_mode) {
            zxfoundation::sched::strandctx::setup_strand_context_kernel_thread(
                s, request.entry, request.arg);
        } else {
            // User-process first-switch: uregs already populated by the caller.
            zxfoundation::sched::strandctx::setup_strand_context_user(
                s, request.uregs, owner);
        }

        if (request.enqueue) {
            enqueue_strand(s);
        }

        return s;
    }

    auto dequeue_domain(domain* d) noexcept -> void {
        dequeue_strand(&d->primary_strand);
    }

    /// @brief Quiesce every strand owned by a domain before identity teardown.
    /// @param[in] d Domain entering the dying state.
    /// @return Success when no strand is running and all runnable strands are removed.
    [[nodiscard]] auto quiesce_domain(domain& d) noexcept
        -> std::expected<void, lib::kernel_error> {
        auto* current = static_cast<domain*>(
            arch::s390x::cpu::lowcore::current_domain_ptr());
        if (current == &d) {
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::busy));
        }
        for (u32 i = 0; i < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++i) {
            const strand* rq_curr = detail::g_rqs[i].curr;
            if (rq_curr && static_cast<domain*>(rq_curr->owner) == &d) {
                return std::unexpected(lib::kernel_error::from_driver(
                    lib::driver_error::busy));
            }
        }
        if (d.primary_strand.on_rq) dequeue_strand(&d.primary_strand);
        for (auto it = d.strands.begin(); it != d.strands.end();) {
            auto* node = *it++;
            auto* body = lib::container_of<zxfoundation::dgp::domain::strand_body,
                lib::list_node, &zxfoundation::dgp::domain::strand_body::strand_list>(node);

            auto* strand_ptr = body->self;
            if (strand_ptr->on_rq) dequeue_strand(strand_ptr);
        }
        return {};
    }

    auto yield_forfeit(strand& s) noexcept -> void {
        auto& rq = this_rq();
        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        rq.lock.lock();
        if (s.body) {
            const u64 now_ns =
                zxfoundation::sys::time::monotonic_now()
                    .nanoseconds_since_epoch();
            detail::runtime_charge(rq, s, now_ns);
            // Forfeit only toward waiters: an empty queue has nothing to
            // hand the slice to.
            if (rq.nr_running != 0U) {
                const u64 avg = zxfoundation::sched::fair::avg_vruntime(
                    rq.min_vruntime, rq.avg_vruntime_acc,
                    rq.avg_weight_acc);
                if (zxfoundation::sched::fair::entity_eligible(s.se, avg)) {
                    s.se.vruntime = s.se.deadline;
                    s.se.deadline = s.se.vruntime + s.se.slice;
                    s.se.lag = static_cast<i64>(s.se.vruntime)
                        - static_cast<i64>(avg);
                }
            }
        }
        rq.lock.unlock();
        arch::s390x::cpu::irq::irq_restore(flags);
    }

    auto schedule() noexcept -> void {
        auto& rq = this_rq();

        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        detail::validate_scheduler_irq_context();
        rq.lock.lock();

        strand* prev = rq.curr;
        strand* next = nullptr;

        if (prev && prev != rq.idle_strand && prev->body) {
            detail::runtime_charge(
                rq, *prev,
                zxfoundation::sys::time::monotonic_now()
                    .nanoseconds_since_epoch());
        }

        if (prev) {
            auto* prev_dom = static_cast<domain*>(prev->owner);
            if (prev_dom) {
                prev_dom->flags &= ~domain_flags::need_resched;
            }

            if (prev_dom
                && admit::can_admit_domain(prev_dom)
                && (prev->substate == sched_substate::running || prev->substate == sched_substate::runnable)
                && !prev->on_rq
                && prev != rq.idle_strand
                && (prev->flags & zxfoundation::dgp::domain::strand_flags::dead)
                    == zxfoundation::dgp::domain::strand_flags::none) {
                prev->substate = sched_substate::runnable;
                auto enqueue_result = classed::enqueue(rq, *prev);
                if (!enqueue_result) [[unlikely]] {
                    rq.lock.unlock();
                    arch::s390x::cpu::irq::irq_restore(flags);
                    zxfoundation::sys::syschk::syschk_fatal(
                        enqueue_result.error(),
                        "sched: scheduler class requeue denied");
                }
            } else if (prev != rq.idle_strand
                && prev->on_rq
                && prev->substate != sched_substate::running
                && prev->substate != sched_substate::runnable) {
                classed::dequeue(rq, *prev);
            } else if (prev != rq.idle_strand && !prev->on_rq
                && prev->substate == sched_substate::running) {
                prev->substate = sched_substate::idle;
            }
        }

        while (true) {
            next = classed::select_next(rq);
            if (next == rq.idle_strand) {
                next = nullptr;
                break;
            }
            if (!next) break;

            auto* candidate_owner = static_cast<domain*>(next->owner);
            const bool dead_strand =
                (next->flags & zxfoundation::dgp::domain::strand_flags::dead)
                != zxfoundation::dgp::domain::strand_flags::none;
            const bool revoked = candidate_owner
                && candidate_owner->pending_rights_revoke_raw != 0U;
            if (dead_strand
                || !admit::can_admit_domain(candidate_owner)
                || revoked) [[unlikely]] {
                classed::dequeue(rq, *next);
                next->substate = sched_substate::idle;
                continue;
            }
            break;
        }

        if (!next && detail::g_sched_initialized) {
            if (rq_snapshot::steal_epoch_slot() == rq.cpu_id) {
                rq_snapshot::steal_epoch_advance(rq.cpu_id);
                next = detail::try_steal_strand_locked(rq);
            }
            if (next) {
                auto enqueue_result = classed::enqueue(rq, *next);
                if (!enqueue_result) [[unlikely]] {
                    rq.lock.unlock();
                    arch::s390x::cpu::irq::irq_restore(flags);
                    zxfoundation::sys::syschk::syschk_fatal(
                        enqueue_result.error(),
                        "sched: scheduler class migrated enqueue denied");
                }
                next = classed::select_next(rq);
                if (next == rq.idle_strand) next = nullptr;
            }
        }

        if (!next) {
            next = rq.idle_strand;
        }

        auto* next_owner = static_cast<domain*>(next->owner);
        if (next != rq.idle_strand
            && (!admit::can_admit_domain(next_owner)
                || (next_owner && next_owner->pending_rights_revoke_raw != 0U))) [[unlikely]] {
            classed::dequeue(rq, *next);
            next->substate = sched_substate::idle;
            next = rq.idle_strand;
            next_owner = static_cast<domain*>(next->owner);
        }

        if (prev && prev->body && prev != rq.idle_strand) {
            u64 cr15_out = 0;
            arch::s390x::cpu::ctlreg::crread<15, 15>(cr15_out);
            arch::s390x::cpu::pcss::stack_save(
                prev->body->linkage_stack, cr15_out);
        }

        if (next != rq.idle_strand && next && next->body) {
            arch::s390x::cpu::ctlreg::crwrite<15, 15>(
                arch::s390x::cpu::pcss::stack_load(
                    next->body->linkage_stack));
        }

        if (next == prev) {
            if (next != rq.idle_strand) {
                if (next->on_rq) {
                    classed::dequeue(rq, *next);
                }
                next->substate = sched_substate::running;
            }
            rq_snapshot::publish(rq);
            rq.lock.unlock();
            arch::s390x::cpu::irq::irq_restore(flags);
            if (next_owner) {
                if (!(next->body != nullptr && next->body->in_pc != 0U)) {
                    if (!(next->body != nullptr && next->body->in_pc != 0U)) {
                        zxfoundation::dgp::signal::signal_deliver_all(*next_owner);
                    }
                }
            }
            return;
        }

        if (next != rq.idle_strand && next->on_rq) {
            classed::dequeue(rq, *next);
        }

        if (next == rq.idle_strand && (prev == nullptr || prev != rq.idle_strand)) {
            nohz::idle_enter(rq.nohz_flags, nohz::the_ops);
        } else if (next != rq.idle_strand && prev == rq.idle_strand) {
            nohz::idle_exit(rq.nohz_flags, nohz::the_ops);
        }

        if (next != rq.idle_strand) {
            next->substate = sched_substate::running;
            if (next_owner && next_owner->signal_queue.has_pending()) {
                next_owner->flags |= domain_flags::need_resched;
            }
            const u64 calibrated_slice = fair::sched_slice(next->se, rq.nr_running);
            next->se.slice = calibrated_slice;
            detail::cputimer_arm_ns(calibrated_slice);
            if (next->body) {
                next->body->sched_stamp_ns =
                    zxfoundation::sys::time::monotonic_now()
                        .nanoseconds_since_epoch();
            }
        } else {
            arch::s390x::cpu::cputimer::spt(
                zxfoundation::sys::timer::TICK_INTERVAL_TOD);
            detail::g_cputimer_armed_tod[detail::pcp_cpu_id()] =
                zxfoundation::sys::timer::TICK_INTERVAL_TOD;
        }

        rq.curr = next;
        rq_snapshot::publish(rq);

        rq.lock.unlock();
        rq.context_lock.lock();

        auto* prev_owner = prev ? static_cast<domain*>(prev->owner) : nullptr;
        const bool next_in_pc = (next != nullptr && next->body != nullptr
                                 && next->body->in_pc != 0U);
        if (next_in_pc) {
            arch::s390x::cpu::ctlreg::crwrite<1, 1>(next->body->pc_cr1);
        } else if (next_owner
            && next_owner->asce.val != 0
            && (!prev_owner || next_owner->asce.val != prev_owner->asce.val)) {
            detail::validate_scheduler_irq_context(next_owner->ko.id);
            const u64 next_asce_val = next_owner->asce.val;
            arch::s390x::cpu::ctlreg::crwrite<1, 1>(next_asce_val);
            auto* lc = arch::s390x::cpu::lowcore::get_current();
            lc->set<"user_asce">(next_asce_val);
            detail::record_asce_attachment(*next_owner);
        }
        if (next_owner) {
            arch::s390x::cpu::aste::art_context_load(next_owner->art);

            {
                auto* nucleus = zxfoundation::dgp::domain::domain_nucleus();
                arch::s390x::cpu::ctlreg::crwrite<13, 13>(
                    nucleus ? nucleus->asce.val : 0ULL);
            }
            const u64 recomputed =
                arch::s390x::cpu::aste::aste_origin(next_owner->pc_asn) & 0x7FFFFFC0ULL;
            const u64 recorded = next_owner->pc_tables.aste_phys & 0x7FFFFFC0ULL;
            const u64 pasteo = (recorded != 0ULL) ? recorded : recomputed;
            arch::s390x::cpu::ctlreg::crwrite<5, 5>(
                next_in_pc ? next->body->pc_cr5 : pasteo);
            u64 cr14 = 0;
            arch::s390x::cpu::ctlreg::crread<14, 14>(cr14);
            arch::s390x::cpu::ctlreg::crwrite<14, 14>(
                (cr14 | 0x0008'0000ULL) |
                (arch::s390x::cpu::aste::aft_origin() & 0x7FFFFULL));
            if (next_in_pc) {
                arch::s390x::cpu::ctlreg::crwrite<4, 4>(next->body->pc_cr4);
                arch::s390x::cpu::ctlreg::crwrite<3, 3>(next->body->pc_cr3);
            } else {
                arch::s390x::cpu::ctlreg::crwrite<4, 4>(
                    static_cast<u64>(next_owner->pc_asn));
                {
                    u64 cr3 = 0;
                    arch::s390x::cpu::ctlreg::crread<3, 3>(cr3);
                    arch::s390x::cpu::ctlreg::crwrite<3, 3>(
                        (cr3 & 0xFFFF'FFFF'FFFF'0000ULL)
                            | static_cast<u64>(next_owner->pc_asn));
                }
            }
        }

        rq.context_lock.unlock();

        if (next != rq.idle_strand && next->ksp == next->kstack_top) {
            zxfoundation::sys::syschk::syschk_fatal(
                lib::kernel_error::from_generic(lib::generic_error::internal_error),
                "sched: frameless switch-in id={} owner={} ksp={:#x} ktop={:#x} sub={} onrq={} body={:#x}",
                next->ko.id,
                next->owner ? static_cast<domain*>(next->owner)->ko.id : 0U,
                next->ksp, next->kstack_top,
                static_cast<u32>(next->substate), next->on_rq,
                reinterpret_cast<u64>(next->body));
        }

        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        pcp.current_strand = reinterpret_cast<u64>(next);

        if (prev && ((prev->flags
                & zxfoundation::dgp::domain::strand_flags::dead)
                != zxfoundation::dgp::domain::strand_flags::none)) {
            pcp.pending_dead_strand = reinterpret_cast<u64>(prev);
        }

        auto* switched_from_owner = __switch_to_asm(prev, next);

        arch::s390x::cpu::irq::irq_restore(flags);

        if (const u64 pending = pcp.pending_dead_strand; pending != 0U) {
            pcp.pending_dead_strand = 0U;
            zxfoundation::dgp::domain::release_dead_strand_stack(
                *reinterpret_cast<strand*>(pending));
        }

        zxfoundation::sync::rcu::quiescent_state();
        softirq::raise_balance_local(softirq::balance_kind::periodic);

        if (next_owner) {
            zxfoundation::dgp::signal::signal_deliver_all(*next_owner);
        }

        if (switched_from_owner &&
            switched_from_owner->ko.state == lifecycle_state::dying) {
            zxfoundation::dgp::gate::destroy_for_domain(switched_from_owner->ko.id);
            zxfoundation::dgp::portal::destroy_for_domain(switched_from_owner->ko.id);
            detail::domain_reap(*switched_from_owner);
        }
    }

    [[noreturn]] auto strand_trampoline_exit() noexcept -> void {
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto* current = reinterpret_cast<strand*>(pcp.current_strand);
        if (current) {
            const auto irq_flags = arch::s390x::cpu::irq::irq_save_disable();
            auto& rq = this_rq();
            rq.lock.lock();
            current->substate = sched_substate::blocked;
            current->flags = current->flags
                | zxfoundation::dgp::domain::strand_flags::dead;
            rq.lock.unlock();
            arch::s390x::cpu::irq::irq_restore(irq_flags);
            discard_value zxfoundation::scoms::kobject::mark_destroyed(current);
            if (auto* owner = static_cast<domain*>(current->owner)) {
                owner->flags = owner->flags | domain_flags::need_resched;
            }
        }

        for (;;) {
            schedule();
            arch::s390x::cpu::psw::enabled_wait();
        }
    }

    /// @brief CPU timer quantum enforcement — fires when a strand's quantum expires.
    auto cpu_timer_tick() noexcept -> void {
        if (!detail::g_sched_initialized) return;


        auto& rq = this_rq();

        auto* curr = rq.curr;
        if (!curr || curr == rq.idle_strand) {
            arch::s390x::cpu::cputimer::spt(zxfoundation::sys::timer::TICK_INTERVAL_TOD);
            detail::g_cputimer_armed_tod[detail::pcp_cpu_id()] =
                zxfoundation::sys::timer::TICK_INTERVAL_TOD;
            drivers::watchdog::heartbeat_bump();
            return;
        }

        const u64 now_ns =
            zxfoundation::sys::time::monotonic_now().nanoseconds_since_epoch();

        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        rq.lock.lock();
        detail::runtime_charge(rq, *curr, now_ns);

        const bool deadline_expired = (curr->se.vruntime >= curr->se.deadline);

        namespace pelt = zxfoundation::sched::pelt;
        if (curr->body) {
            pelt::update_entity_load_avg(curr->body->pelt_avg, now_ns, curr->se.weight, 1U, 1U);
        }
        pelt::update_rq_load_avg(rq.cfs_avg, now_ns, (rq.nr_running > 0U) ? 1U : 0U);

        const u32 total = rq.nr_running;
        topology::refresh_class_load(rq.cpu_id, total, 0U, 0U);
        rq_snapshot::publish(rq);

        rq.lock.unlock();

        if (deadline_expired) {
            auto* owner = static_cast<domain*>(curr->owner);
            owner->flags = owner->flags | domain_flags::need_resched;
            curr->se.deadline = curr->se.vruntime +
                (curr->se.slice * 1024 / curr->se.weight);
            curr->se.slice = fair::sched_slice(curr->se, rq.nr_running);
        }

        detail::cputimer_arm_ns(curr->se.slice);
        arch::s390x::cpu::irq::irq_restore(flags);
        drivers::watchdog::heartbeat_bump();
    }

    /// @brief CPU timer IRQ handler — dispatches vector EXT_IRQ_CPU_TIMER.
    auto irq_handler_cpu_timer(
        u16 irq,
        arch::s390x::cpu::irq::irq_frame* frame,
        void* data
    ) noexcept -> void {
        discard_value irq; discard_value frame; discard_value data;
        cpu_timer_tick();
    }

    /// @brief Periodic scheduler tick
    auto scheduler_tick() noexcept -> void {
        if (!detail::g_sched_initialized) return;

        auto& rq = this_rq();

        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        detail::validate_scheduler_irq_context();
        rq.lock.lock();

        auto* curr = rq.curr;
        if (!curr || curr == rq.idle_strand) {
            namespace pelt = zxfoundation::sched::pelt;
            const u64 now_ns = zxfoundation::sys::time::monotonic_now().nanoseconds_since_epoch();
            pelt::update_rq_load_avg(rq.cfs_avg, now_ns, 0U);
            topology::refresh_class_load(rq.cpu_id, 0U, 0U, 0U);
            rq_snapshot::publish(rq);
            rq.lock.unlock();
            arch::s390x::cpu::irq::irq_restore(flags);
            zxfoundation::sync::rcu::tick();
            softirq::raise_balance_local(softirq::balance_kind::nohz_idle);
            drivers::watchdog::heartbeat_bump();
            return;
        }

        classed::tick(rq, *curr);

        namespace pelt = zxfoundation::sched::pelt;
        const u64 now_ns = zxfoundation::sys::time::monotonic_now().nanoseconds_since_epoch();
        if (curr->body) {
            pelt::update_entity_load_avg(curr->body->pelt_avg, now_ns, curr->se.weight, 1U, 1U);
        }
        pelt::update_rq_load_avg(rq.cfs_avg, now_ns, (rq.nr_running > 0U) ? 1U : 0U);

        const u32 total = rq.nr_running;
        topology::refresh_class_load(rq.cpu_id, total, 0U, 0U);
        rq_snapshot::publish(rq);

        if (auto* owner = static_cast<domain*>(curr->owner);
            owner && owner->pending_rights_revoke_raw != 0U) {
            classed::dequeue(rq, *curr);
            curr->substate = sched_substate::idle;
            owner->flags |= domain_flags::need_resched;
        }

        rq.lock.unlock();
        arch::s390x::cpu::irq::irq_restore(flags);

        zxfoundation::sync::rcu::tick();
        softirq::raise_balance_local(softirq::balance_kind::periodic);
        drivers::watchdog::heartbeat_bump();
    }

    [[nodiscard]] auto need_resched() noexcept -> bool {
        auto* dom = static_cast<domain*>(
            arch::s390x::cpu::lowcore::current_domain_ptr());
        if (!dom) return false;
        return !!(dom->flags & domain_flags::need_resched);
    }

    /// @brief Mark the currently executing domain as needing rescheduling.
    auto sched_resched_current() noexcept -> void {
        detail::set_need_resched();
    }

    auto preempt_schedule() noexcept -> void {
        if (!detail::g_sched_initialized) return;
        do {
            context::preempt_disable();            // count>0 shields the switch
            schedule();
            context::preempt_enable_no_resched();  // loop re-arms, no recursion
        } while (need_resched());
    }

    /// @brief preempt_enable count→0 seam (Linux __preempt_schedule).
    extern "C" auto zx_preempt_schedule_hook() noexcept -> void {
        if (!detail::g_sched_initialized) return;
        preempt_schedule();
    }

    [[nodiscard]] auto sched_active() noexcept -> bool {
        return detail::g_sched_initialized;
    }

    auto zi_sched_init(const sys::zinit::zi_desc&) noexcept -> sys::zinit::zi_result {
        using namespace detail;

        topology::topology_init();

        for (u32 i = 0; i < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++i) {
            g_rqs[i].cpu_id = static_cast<u16>(i);
        }

        auto* idle_s = idle::init_idle_strand(0);
        if (!idle_s) {
            zxfoundation::sys::printk::perr("sched: cannot create idle strand");
            return std::unexpected(lib::kernel_error::from_memory(lib::memory_error::out_of_memory));
        }

        auto& rq = g_rqs[0];
        rq.idle_strand = idle_s;
        rq.curr = rq.idle_strand;

        auto& lc = *arch::s390x::cpu::lowcore::get_current();
        auto* nucleus = zxfoundation::dgp::domain::domain_nucleus();
        lc.set<"current_domain"> (nucleus ? reinterpret_cast<u64>(nucleus) : 0U);
        lc.set<"kernel_stack">   (idle_s->kstack_top);
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        pcp.attached.publish(0U, lc.get<"current_domain">(), nucleus ? nucleus->ko.id : 0U,
            nucleus ? static_cast<u8>(nucleus->ko.state) : 0U);
        pcp.current_strand = reinterpret_cast<u64>(rq.curr);
        pcp.kernel_stack = idle_s->kstack_top;

        cpupri::init();
        cpudl::init();
        rt::bw::bw_init(rq.rt_bw, 0U);
        {
            const auto* rec = topology::record_for(0U);
            const u8 node = (rec && rec->numa_node < zxfoundation::memory::numa::MAX_NUMA_NODES) ? rec->numa_node : 0xFFU;
            sd::tree_build(0U, rec, topology::node_mask_of(node), rq.sd_tree);
        }

        {
            using namespace zxfoundation::sys::timer;
            auto& t = g_bsp_tick_timer;
            t.deadline_tod = arch::s390x::cpu::clock::stck() + TICK_INTERVAL_TOD;
            t.interval_tod = TICK_INTERVAL_TOD;
            t.callback     = [](void*) noexcept -> void {
                zxfoundation::sys::time::tick_advance_wall(TICK_INTERVAL_NS);
                zxfoundation::sched::scheduler_tick();
            };
            t.data         = nullptr;
            t.mode         = timer_mode::periodic;
            discard_value timer_add(t);
        }

        rq.online.store(1, std::memory_order::release);
        rq_snapshot::publish(rq);

        arch::s390x::cpu::ar::purge_alb();
        g_sched_initialized = true;

        {
            namespace irq = zxfoundation::sys::irq;
            constexpr u16 cpu_timer_irq = static_cast<u16>(
                irq::IRQ_BASE_EXT + irq::EXT_IRQ_CPU_TIMER);
            auto r = irq::irq_register(
                cpu_timer_irq, zxfoundation::sched::irq_handler_cpu_timer, nullptr, irq::IRQF_FAST);
            if (!r) [[unlikely]] {
                zxfoundation::sys::syschk::syschk_warning(
                    r.error(),
                    "sched: failed to register CPU-timer IRQ {:#06x}",
                    cpu_timer_irq);
            }
        }

        discard_value softirq::open_sched_softirq();

        zxfoundation::sys::printk::pinfo("sched: idle strand id={} on cpu 0, scheduler ready",
              idle_s->ko.id);
        return {};
    }
    __zinit__(sched, p00, zi_sched_init, fatal)

    auto sched_init_ap(u16 cpu_id) noexcept -> void {
        auto result = scheduler_cpu_attach(cpu_id);
        if (!result) {
            zxfoundation::sys::syschk::syschk_fatal(
                result.error(),
                "sched: cannot attach cpu {}",
                cpu_id);
        }
    }

    [[noreturn]] auto idle_enter() noexcept -> void {
        while (true) {
            schedule();
            if (this_rq().nr_running == 0U && !need_resched()) {
                arch::s390x::cpu::psw::enabled_wait();
            }
        }
    }

    /// @brief Notify the scheduler that a CPU has come online.
    /// @param[in] cpu_id  Logical CPU ID to attach.
    [[nodiscard]] auto scheduler_cpu_attach(u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        using namespace detail;

        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));

        if (g_rqs[cpu_id].idle_strand != nullptr)
            return {};

        auto* idle_s = idle::init_idle_strand(cpu_id);
        if (!idle_s)
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::out_of_memory));

        auto& rq = g_rqs[cpu_id];

        auto& lc = *arch::s390x::cpu::lowcore::get_current();
        auto* nucleus = zxfoundation::dgp::domain::domain_nucleus();
        lc.set<"current_domain"> (nucleus ? reinterpret_cast<u64>(nucleus) : 0U);
        lc.set<"kernel_stack">   (idle_s->kstack_top);
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        pcp.attached.publish(0U, lc.get<"current_domain">(), nucleus ? nucleus->ko.id : 0U,
            nucleus ? static_cast<u8>(nucleus->ko.state) : 0U);
        pcp.current_strand = 0;
        pcp.kernel_stack = idle_s->kstack_top;

        cpupri::init();
        cpudl::init();
        rt::bw::bw_init(rq.rt_bw, cpu_id);
        {
            const auto* rec = topology::record_for(cpu_id);
            const u8 node = (rec && rec->numa_node < zxfoundation::memory::numa::MAX_NUMA_NODES) ? rec->numa_node : 0xFFU;
            sd::tree_build(cpu_id, rec, topology::node_mask_of(node), rq.sd_tree);
        }

        {
            using namespace zxfoundation::sys::timer;
            init_ap(cpu_id);

            auto& t = detail::g_ap_tick_timers[cpu_id];
            t.deadline_tod = arch::s390x::cpu::clock::stck() + TICK_INTERVAL_TOD;
            t.interval_tod = TICK_INTERVAL_TOD;
            t.callback     = [](void*) noexcept -> void {
                zxfoundation::sched::scheduler_tick();
            };
            t.data         = nullptr;
            t.mode         = timer_mode::periodic;
            discard_value timer_add(t);
        }

        {
            const auto flags = arch::s390x::cpu::irq::irq_save_disable();
            rq.lock.lock();
            rq.idle_strand = idle_s;
            rq.curr = rq.idle_strand;
            rq.online.store(1, std::memory_order::release);
            rq_snapshot::publish(rq);
            rq.lock.unlock();
            arch::s390x::cpu::irq::irq_restore(flags);
        }

        arch::s390x::cpu::ar::purge_alb();
        return {};
    }

    /// @brief Notify the scheduler that a CPU is going offline.
    /// @param[in] cpu_id Logical CPU ID going offline.
    /// @return Success or kernel_error.
    [[nodiscard]] auto scheduler_cpu_detach(u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        using namespace detail;

        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));

        auto& rq = g_rqs[cpu_id];
        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        validate_scheduler_irq_context();

        u16 dest_cpu = static_cast<u16>(zxfoundation::base::config::CONFIG_ZX_MAX_CPUS);
        for (u16 i = 0; i < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++i) {
            if (i != cpu_id && arch::s390x::cpu::cpumask::cpu_online(i)) {
                dest_cpu = i;
                break;
            }
        }
        if (dest_cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) {
            arch::s390x::cpu::irq::irq_restore(flags);
            return std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }

        auto& dest_rq = g_rqs[dest_cpu];
        run_queue* first = &rq;
        run_queue* second = &dest_rq;
        if (dest_cpu < cpu_id) {
            first = &dest_rq;
            second = &rq;
        }
        first->lock.lock();
        second->lock.lock();

        strand* old_idle = rq.idle_strand;

        for (strand& s : fair::iter(rq)) {
            classed::dequeue(rq, s);
            s.cpu = dest_cpu;
            s.on_rq = false;
            s.substate = sched_substate::runnable;
                auto enqueue_result = classed::enqueue(dest_rq, s);
                if (!enqueue_result) [[unlikely]] {
                    sys::syschk::syschk_fatal(
                        enqueue_result.error(),
                        "sched: fair strand migration during CPU detach failed");
                }
        }

        auto migrate_list = [&](lib::list_head& queue) noexcept -> void {
            auto* node = queue.sentinel.next;
            while (node != &queue.sentinel) {
                auto* next = node->next;
                auto* s = lib::container_of<strand, lib::list_node,
                    &strand::sched_link>(node);
                classed::dequeue(rq, *s);
                s->cpu = dest_cpu;
                s->on_rq = false;
                s->substate = sched_substate::runnable;
                auto enqueue_result = classed::enqueue(dest_rq, *s);
                if (!enqueue_result) [[unlikely]] {
                    sys::syschk::syschk_fatal(
                        enqueue_result.error(),
                        "sched: class strand migration during CPU detach failed");
                }
                node = next;
            }
        };
        migrate_list(rq.rt_queue);
        migrate_list(rq.deadline_queue);

        rq_snapshot::publish(dest_rq);
        cpupri::pri_set(cpu_id, cpupri::pri_top(cpu_id), cpupri::CPUPRI_INVALID);
        cpudl::clear_dl(cpu_id);

        rq.idle_strand = nullptr;
        rq.curr = nullptr;
        rq.online.store(0, std::memory_order::release);
        rq_snapshot::publish(rq);

        second->lock.unlock();
        first->lock.unlock();
        arch::s390x::cpu::irq::irq_restore(flags);

        if (old_idle) {
            discard_value idle::deinit_idle_strand(*old_idle);
        }
        return {};
    }

} // namespace zxfoundation::sched

extern "C" [[noreturn]] auto sched_strand_trampoline_exit() noexcept -> void {
    zxfoundation::sched::strand_trampoline_exit();
}
