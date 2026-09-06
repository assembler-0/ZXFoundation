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
import zxfoundation.sched.rt.core;
import zxfoundation.smp.csd.core;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.aste;
import arch.s390x.cpu.ar;
import arch.s390x.cpu.clock;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.ipi;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.cputimer;
import lib.error;
import lib.format;
import lib.rbtree;
import lib.list;
import lib.softirq.core;
import lib.cpumask;
import lib.container_of;
import drivers.watchdog.core;
import lib.stl;

namespace zxfoundation::sched::detail {

    using namespace zxfoundation::dgp::domain;
    using namespace zxfoundation::sys::printk;

    no_export std::array<run_queue, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_rqs{};
    no_export bool g_sched_initialized{false};
    no_export sys::timer::kernel_timer g_bsp_tick_timer{};
    no_export std::array<sys::timer::kernel_timer, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_ap_tick_timers{};

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

        discard_value zxfoundation::dgp::domain::domain_destroy(d.ko.id);
    }

    auto record_asce_attachment(domain& d) noexcept -> void {
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        pcp.attached_asce = d.asce.val;
        pcp.attached_domain_id = d.ko.id;
        pcp.attached_dgp_state = static_cast<u8>(d.ko.state);
        auto& rq = detail::g_rqs[pcp.cpu_id];
        rq.balance_attached_asce.store(pcp.attached_asce, std::memory_order::release);
        rq.balance_attached_domain_id.store(pcp.attached_domain_id, std::memory_order::release);
        rq.balance_attached_dgp_state.store(pcp.attached_dgp_state, std::memory_order::release);
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
        second.lock.lock();

        if (!cpumask::cpu_online(busiest->cpu_id) || busiest->nr_running <= 1) {
            first.lock.unlock();
            if (&second != &first) second.lock.unlock();
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
        first.lock.unlock();
        if (&second != &first) second.lock.unlock();
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
    /// @param[in] request Executor-style creation request selecting one of three
    ///            strand kinds:
    ///            - **Kernel-thread**: @c entry != nullptr, @c uregs == nullptr.
    ///              The strand's first @c __switch_to_asm dispatches via
    ///              @c ret_from_fork to @c domain_trampoline, which invokes
    ///              @c entry(@c arg) on the kernel stack and then tail-calls
    ///              @c sched_strand_trampoline_exit (the @c do_exit analog).
    ///            - **User-process**: @c entry == nullptr, @c uregs != nullptr
    ///              with @c psw_addr != 0. The strand's first
    ///              @c __switch_to_asm dispatches via @c ret_from_fork to
    ///              @c ret_to_user, which LPSWEs into user mode at
    ///              @c uregs->psw_addr with @c uregs->psw_mask.  The AR/VR/FPC
    ///              state from @c uregs is pre-staged in the strand's save
    ///              area so @c __switch_to_asm itself restores them onto the
    ///              CPU before control reaches @c ret_from_fork.
    ///            - **Bare (idle)**: @c entry == nullptr AND @c uregs == nullptr.
    ///              The strand is allocated but no first-switch frame is
    ///              written. @c request.enqueue MUST be false (a bare strand
    ///              with no entry must not be admitted to a run queue —
    ///              scheduling it would LPSWE into garbage).  Useful for
    ///              SCOMS bookkeeping paths that need a strand object but
    ///              have no runnable work yet.
    /// @return Pointer to the created strand, or @c kernel_error if validation,
    ///         allocation, or admission fails.
    ///
    /// @warning The kernel-thread and user-process modes are mutually exclusive:
    ///          @c entry != nullptr AND @c uregs != nullptr is rejected with
    ///          @c invalid_arg.  The bare mode requires @c enqueue == false.
    ///
    /// @details The kthread vs user split mirrors Linux @c copy_thread:
    ///          - Kernel-thread: @c entry(arg) runs on the new strand's kernel
    ///            stack and tail-calls @c sched_strand_trampoline_exit on return.
    ///          - User-process: the new strand's first context switch LPSWEs
    ///            into user mode at @c uregs->psw_addr with @c uregs->psw_mask.
    [[nodiscard]] auto strand_spawn(
        const zxfoundation::dgp::domain::strand_create_request& request
    ) noexcept -> std::expected<zxfoundation::dgp::domain::strand*, lib::kernel_error> {
        if (request.stack_order != 2U) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        const bool kthread_mode = (request.entry != nullptr);
        const bool user_mode    = (request.uregs != nullptr);

        // Mutual-exclusion: entry and uregs cannot both be set.
        if (kthread_mode && user_mode) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        // Bare mode (neither set) requires enqueue=false — a strand with no
        // first-switch frame must never be admitted to a run queue.
        if (!kthread_mode && !user_mode && request.enqueue) {
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
        } else if (user_mode) {
            // User-process first-switch: uregs already populated by the caller.
            zxfoundation::sched::strandctx::setup_strand_context_user(
                s, request.uregs, owner);
        }
        // else: bare mode — no first-switch frame written.

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
        if (current == &d || d.primary_strand.substate == sched_substate::running) {
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::busy));
        }
        if (d.primary_strand.on_rq) dequeue_strand(&d.primary_strand);
        for (auto it = d.strands.begin(); it != d.strands.end();) {
            auto* node = *it++;
            auto* body = lib::container_of<zxfoundation::dgp::domain::strand_body,
                lib::list_node, &zxfoundation::dgp::domain::strand_body::strand_list>(node);
            auto* strand_ptr = reinterpret_cast<strand*>(
                reinterpret_cast<u8*>(body) - __builtin_offsetof(strand, body));
            if (strand_ptr->substate == sched_substate::running) {
                return std::unexpected(lib::kernel_error::from_driver(
                    lib::driver_error::busy));
            }
            if (strand_ptr->on_rq) dequeue_strand(strand_ptr);
        }
        return {};
    }

    auto schedule() noexcept -> void {
        auto& rq = this_rq();

        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        detail::validate_scheduler_irq_context();
        rq.lock.lock();

        strand* prev = rq.curr;
        strand* next = nullptr;

        if (prev) {
            auto* prev_dom = static_cast<domain*>(prev->owner);
            if (prev_dom) {
                prev_dom->flags &= ~domain_flags::need_resched;
            }

            if (prev_dom
                && admit::can_admit_domain(prev_dom)
                && (prev->substate == sched_substate::running || prev->substate == sched_substate::runnable)
                && !prev->on_rq
                && prev != rq.idle_strand) {
                prev->substate = sched_substate::runnable;
                auto enqueue_result = classed::enqueue(rq, *prev);
                if (!enqueue_result) [[unlikely]] {
                    rq.lock.unlock();
                    arch::s390x::cpu::irq::irq_restore(flags);
                    zxfoundation::sys::syschk::syschk_fatal(
                        enqueue_result.error(),
                        "sched: scheduler class requeue denied");
                }
            }
        }

        next = classed::select_next(rq);
        if (next == rq.idle_strand) next = nullptr;

        if (!next && detail::g_sched_initialized) {
            next = detail::try_steal_strand_locked(rq);
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
        if (!admit::can_admit_domain(next_owner)) [[unlikely]] {
            next = rq.idle_strand;
            next_owner = static_cast<domain*>(next->owner);
        }

        if (next != rq.idle_strand && next_owner
            && next_owner->pending_rights_revoke_raw != 0U) {
            classed::dequeue(rq, *next);
            next->substate = sched_substate::idle;
            next = rq.idle_strand;
            next_owner = static_cast<domain*>(next->owner);
        }

        if (next == prev) {
            if (next != rq.idle_strand) {
                next->substate = sched_substate::running;
            }
            rq_snapshot::publish(rq);
            rq.lock.unlock();
            arch::s390x::cpu::irq::irq_restore(flags);
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
            arch::s390x::cpu::cputimer::spt(calibrated_slice);
        } else {
            arch::s390x::cpu::cputimer::spt(
                zxfoundation::sys::timer::TICK_INTERVAL_TOD);
        }

        rq.curr = next;
        rq_snapshot::publish(rq);

        rq.lock.unlock();
        rq.context_lock.lock();

        auto* prev_owner = prev ? static_cast<domain*>(prev->owner) : nullptr;
        if (next_owner
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
        }

        rq.context_lock.unlock();

        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        pcp.current_strand = reinterpret_cast<u64>(next);

        auto* switched_from_owner = __switch_to_asm(prev, next);

        arch::s390x::cpu::irq::irq_restore(flags);

        softirq::raise_balance_local(softirq::balance_kind::periodic);

        if (next_owner) {
            zxfoundation::dgp::signal::signal_deliver_all(*next_owner);
        }

        if (switched_from_owner &&
            switched_from_owner->ko.state == lifecycle_state::dying &&
            (switched_from_owner->flags & domain_flags::idle) == domain_flags::none) {
            zxfoundation::dgp::gate::destroy_for_domain(switched_from_owner->ko.id);
            zxfoundation::dgp::portal::destroy_for_domain(switched_from_owner->ko.id);
            detail::domain_reap(*switched_from_owner);
        }
    }

    [[noreturn]] auto strand_trampoline_exit() noexcept -> void {
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto* current = reinterpret_cast<strand*>(pcp.current_strand);
        if (current) {
            current->on_rq = false;
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
            drivers::watchdog::heartbeat_bump();
            return;
        }

        const u64 remaining = arch::s390x::cpu::cputimer::stpt();
        const u64 elapsed = zxfoundation::sys::timer::TICK_INTERVAL_TOD - remaining;

        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        rq.lock.lock();

        const u64 weighted_delta = (elapsed * 1024) / curr->se.weight;
        curr->se.vruntime += weighted_delta;

        if (curr->se.vruntime > rq.min_vruntime)
            rq.min_vruntime = curr->se.vruntime;

        const bool deadline_expired = (curr->se.vruntime >= curr->se.deadline);

        namespace pelt = zxfoundation::sched::pelt;
        const u64 now_ns = zxfoundation::sys::time::monotonic_now().nanoseconds_since_epoch();
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

        arch::s390x::cpu::cputimer::spt(curr->se.slice);
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
        if (need_resched()) {
            schedule();
        }
    }

    [[nodiscard]] auto sched_active() noexcept -> bool {
        return detail::g_sched_initialized;
    }

    auto sched_init() noexcept -> void {
        using namespace detail;

        topology::topology_init();

        for (u32 i = 0; i < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++i) {
            g_rqs[i].cpu_id = static_cast<u16>(i);
        }

        auto* idle_s = idle::init_idle_strand(0);
        if (!idle_s) {
            zxfoundation::sys::printk::perr("sched: cannot create idle strand");
            return;
        }

        auto& rq = g_rqs[0];
        rq.idle_strand = idle_s;
        rq.curr = rq.idle_strand;

        auto& lc = *arch::s390x::cpu::lowcore::get_current();
        auto* nucleus = zxfoundation::dgp::domain::domain_nucleus();
        lc.set<"current_domain"> (nucleus ? reinterpret_cast<u64>(nucleus) : 0U);
        lc.set<"kernel_stack">   (idle_s->kstack_top);
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        pcp.current_domain = lc.get<"current_domain">();
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
        zxfoundation::smp::csd::enable_cpu(0U);
        arch::s390x::cpu::cpumask::cpu_set_online(0U);
        topology::refresh_cpu_record(0U, 0U, true);
        topology::topology_online_cpu(0U);
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
    }

    auto sched_init_ap(u16 cpu_id) noexcept -> void {
        using namespace detail;

        auto* idle_s = idle::init_idle_strand(cpu_id);
        if (!idle_s) {
            zxfoundation::sys::syschk::syschk_fatal(
                lib::kernel_error::from_memory(lib::memory_error::out_of_memory),
                "sched: cannot create idle strand for cpu {}",
                cpu_id);
        }

        auto& rq = g_rqs[cpu_id];

        auto& lc = *arch::s390x::cpu::lowcore::get_current();
        auto* nucleus = zxfoundation::dgp::domain::domain_nucleus();
        lc.set<"current_domain"> (nucleus ? reinterpret_cast<u64>(nucleus) : 0U);
        lc.set<"kernel_stack">   (idle_s->kstack_top);
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        pcp.current_domain = lc.get<"current_domain">();
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
            const auto ap_flags = arch::s390x::cpu::irq::irq_save_disable();
            rq.lock.lock();
            rq.idle_strand = idle_s;
            rq.curr = rq.idle_strand;
            rq.online.store(1, std::memory_order::release);
            rq_snapshot::publish(rq);
            rq.lock.unlock();
            arch::s390x::cpu::irq::irq_restore(ap_flags);
        }
        arch::s390x::cpu::ar::purge_alb();
        zxfoundation::smp::csd::enable_cpu(cpu_id);
        arch::s390x::cpu::cpumask::cpu_set_online(cpu_id);
        topology::refresh_cpu_record(cpu_id, 0U, true);
        topology::topology_online_cpu(cpu_id);
    }

    [[noreturn]] auto idle_enter() noexcept -> void {
        while (true) {
            schedule();
            arch::s390x::cpu::psw::enabled_wait();
        }
    }

    /// @brief Notify the scheduler that a CPU has come online.
    [[nodiscard]] auto cpu_up(u16 cpu_id) noexcept -> ::std::expected<void, ::lib::kernel_error> {
        using namespace detail;

        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));

        auto* idle_s = idle::init_idle_strand(cpu_id);
        if (!idle_s)
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::out_of_memory));

        auto& rq = g_rqs[cpu_id];
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

        cpupri::init();
        cpudl::init();
        rt::bw::bw_init(rq.rt_bw, cpu_id);
        {
            const auto* rec = topology::record_for(cpu_id);
            const u8 node = (rec && rec->numa_node < zxfoundation::memory::numa::MAX_NUMA_NODES) ? rec->numa_node : 0xFFU;
            sd::tree_build(cpu_id, rec, topology::node_mask_of(node), rq.sd_tree);
        }

        arch::s390x::cpu::ar::purge_alb();
        zxfoundation::smp::csd::enable_cpu(cpu_id);
        arch::s390x::cpu::cpumask::cpu_set_online(cpu_id);
        topology::refresh_cpu_record(cpu_id, 0U, true);
        topology::topology_online_cpu(cpu_id);
        return {};
    }

    /// @brief Notify the scheduler that a CPU is going offline.
    /// @param[in] cpu_id Logical CPU ID going offline.
    /// @return Success or kernel_error.
    [[nodiscard]] auto cpu_down(u16 cpu_id) noexcept -> ::std::expected<void, ::lib::kernel_error> {
        using namespace detail;

        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));

        auto& rq = g_rqs[cpu_id];
        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        validate_scheduler_irq_context();
        rq.lock.lock();

        strand* old_idle = rq.idle_strand;

        for (strand& s : fair::iter(rq)) {
            classed::dequeue(rq, s);
            u16 dest_cpu = 0;
            for (u16 i = 0; i < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++i) {
                if (i != cpu_id && arch::s390x::cpu::cpumask::cpu_online(i)) {
                    dest_cpu = i;
                    break;
                }
            }
            s.cpu = dest_cpu;
            s.on_rq = false;
            s.substate = sched_substate::runnable;
            auto& dest_rq = g_rqs[dest_cpu];
            discard_value classed::enqueue(dest_rq, s);
        }

        cpupri::pri_set(cpu_id, cpupri::pri_top(cpu_id), cpupri::CPUPRI_INVALID);
        cpudl::clear_dl(cpu_id);

        rq.idle_strand = nullptr;
        rq.curr = nullptr;
        rq.online.store(0, std::memory_order::release);
        rq_snapshot::publish(rq);

        rq.lock.unlock();
        arch::s390x::cpu::irq::irq_restore(flags);

        arch::s390x::cpu::cpumask::cpu_clear_online(cpu_id);
        topology::topology_offline_cpu(cpu_id);

        if (old_idle) {
            discard_value idle::deinit_idle_strand(*old_idle);
        }
        return {};
    }

} // namespace zxfoundation::sched

declscope(C) [[noreturn]] auto sched_strand_trampoline_exit() noexcept -> void {
    zxfoundation::sched::strand_trampoline_exit();
}
