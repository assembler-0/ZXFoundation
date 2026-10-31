/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sync/rcu.zcomponent/core.cxx
/// @brief Read-Copy-Update — non-preemptive SMP implementation.

module zxfoundation.sync.rcu.core;
import zxfoundation.sync.rcu.types;
import zxfoundation.sched.context.core;
import zxfoundation.base.types;
import zxfoundation.base.config;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.sync;
import zxfoundation.sys.zinit.core;
import lib.cpumask;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace zxfoundation::sync::rcu::detail {

    using memory_order = std::memory_order;
    using atomic_u64   = std::atomic<u64>;
    using atomic_u32   = std::atomic<u32>;

    static atomic_u64 g_gp_seq{0};    ///< GP sequence counter. Even=idle, odd=active.
    static atomic_u32 g_gp_lock{0};   ///< BSP-side GP state lock.
    static ::lib::cpumask g_qs_mask{};
    static std::atomic<bool> g_initialized{false};

    static auto gp_lock_acquire() noexcept -> void {
        u32 expected = 0;
        while (!g_gp_lock.compare_exchange_weak(
            expected, 1U, memory_order::acquire, memory_order::relaxed))
        {
            expected = 0;
            arch::s390x::cpu::sync::cpu_relax();
        }
    }

    static auto gp_lock_release() noexcept -> void {
        g_gp_lock.store(0U, memory_order::release);
    }

    static auto record_qs(u32 cpu_id) noexcept -> void {
        g_qs_mask.set(static_cast<usize>(cpu_id), memory_order::release);
    }

    static auto all_qs_reported() noexcept -> bool {
        bool all = true;
        arch::s390x::cpu::cpumask::for_each_online_cpu(
            [&](u16 cpu) noexcept -> void {
                if (!g_qs_mask.test(static_cast<usize>(cpu), memory_order::acquire))
                    all = false;
            });
        return all;
    }

    static auto drain_callbacks(
        arch::s390x::cpu::percpu::percpu_data& pcp,
        gp_seq_t completed_gp
    ) noexcept -> void {
        if (!pcp.rcu_pending_cbs) return;
        if (!gp_seq_after(completed_gp, pcp.rcu_pending_gp - 1U)) return;
        u32 drained = 0;
        while (pcp.rcu_pending_cbs && drained < RCU_MAX_DRAIN_PER_TICK) {
            auto* head = static_cast<rcu_head*>(pcp.rcu_pending_cbs);
            pcp.rcu_pending_cbs = head->next;
            if (pcp.rcu_pending_count > 0U) --pcp.rcu_pending_count;
            ++drained;
            if (head->func) head->func(head);
        }
    }

} // namespace zxfoundation::sync::rcu::detail

namespace zxfoundation::sync::rcu {

    auto zi_rcu_init(const sys::zinit::zi_desc&) noexcept -> sys::zinit::zi_result {
        using namespace detail;
        if (g_initialized.load(memory_order::acquire)) return {};
        g_gp_seq.store(0U, memory_order::relaxed);
        g_qs_mask.reset();
        g_initialized.store(true, memory_order::release);
        return {};
    }
    __zinit__(core, p50, zi_rcu_init, fatal)

    /// @brief Enter an RCU read-side critical section.
    auto read_lock() noexcept -> void {
        sched::context::preempt_disable();
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        ++pcp.in_rcu_read_side;
        arch::s390x::cpu::sync::barrier();
    }

    /// @brief Exit an RCU read-side critical section.
    auto read_unlock() noexcept -> void {
        using namespace detail;
        arch::s390x::cpu::sync::barrier();
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        if (pcp.in_rcu_read_side > 0) --pcp.in_rcu_read_side;
        sched::context::preempt_enable();
        if (pcp.in_rcu_read_side == 0U) {
            const gp_seq_t seq = g_gp_seq.load(memory_order::acquire);
            if (gp_in_progress(seq))
                record_qs(pcp.cpu_id);
        }
    }

    /// @brief Enqueue a deferred callback after the next grace period.
    auto call_rcu(rcu_head* head, void(*func)(rcu_head*) noexcept) noexcept -> void {
        using namespace detail;
        head->func = func;
        head->next = nullptr;

        sched::context::preempt_disable();
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();

        const gp_seq_t cur_seq   = g_gp_seq.load(memory_order::acquire);
        const gp_seq_t mature_at = cur_seq + 2U;
        if (pcp.rcu_pending_gp < mature_at)
            pcp.rcu_pending_gp = mature_at;

        head->next = static_cast<rcu_head*>(pcp.rcu_pending_cbs);
        pcp.rcu_pending_cbs = head;
        ++pcp.rcu_pending_count;

        sched::context::preempt_enable();
    }

    /// @brief Block until all pre-existing RCU readers complete.
    auto synchronize_rcu() noexcept -> void {
        using namespace detail;
        auto& current_pcp = arch::s390x::cpu::percpu::this_cpu();
        gp_lock_acquire();
        gp_seq_t seq = g_gp_seq.load(memory_order::acquire);
        if (!gp_in_progress(seq)) {
            g_qs_mask.reset();
            g_gp_seq.store(seq + 1U, memory_order::release);
            seq = seq + 1U;
        }
        if (current_pcp.in_rcu_read_side == 0U)
            record_qs(current_pcp.cpu_id);
        gp_lock_release();

        const gp_seq_t target = seq + 1U;
        for (;;) {
            if (gp_seq_after(g_gp_seq.load(memory_order::acquire), target - 1U))
                return;
            if (arch::s390x::cpu::percpu::this_cpu().cpu_id == 0U) {
                gp_lock_acquire();
                const gp_seq_t active = g_gp_seq.load(memory_order::relaxed);
                if (gp_in_progress(active) && all_qs_reported()) {
                    g_gp_seq.store(active + 1U, memory_order::release);
                    g_qs_mask.reset();
                }
                gp_lock_release();
            }
            arch::s390x::cpu::sync::cpu_relax();
        }
    }

    auto quiescent_state() noexcept -> void {
        using namespace detail;
        if (!g_initialized.load(memory_order::acquire)) return;
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        if (pcp.in_rcu_read_side == 0U
            && gp_in_progress(g_gp_seq.load(memory_order::acquire)))
            record_qs(pcp.cpu_id);
    }

    /// @brief Wait until all enqueued call_rcu callbacks have fired.
    auto barrier() noexcept -> void {
        arch::s390x::cpu::cpumask::for_each_online_cpu(
            [](u16 cpu) noexcept -> void {
                auto* lc = arch::s390x::cpu::lowcore::get_for_cpu(cpu);
                if (!lc) return;
                while (__atomic_load_n(&arch::s390x::cpu::percpu::at(cpu).rcu_pending_count,
                        static_cast<int>(std::memory_order::acquire)) > 0U)
                    arch::s390x::cpu::sync::cpu_relax();
            });
    }

    /// @brief Per-CPU RCU tick: report QS, manage GPs, drain callbacks.
    auto tick() noexcept -> void {
        using namespace detail;
        if (!g_initialized) return;

        auto& pcp         = arch::s390x::cpu::percpu::this_cpu();
        const u32 cpu_id  = pcp.cpu_id;

        const gp_seq_t cur_gp = g_gp_seq.load(memory_order::acquire);
        if (gp_in_progress(cur_gp) && pcp.in_rcu_read_side == 0U)
            record_qs(cpu_id);

        if (cpu_id == 0U) {
            gp_lock_acquire();
            const gp_seq_t gp = g_gp_seq.load(memory_order::relaxed);
            if (gp_in_progress(gp)) {
                if (all_qs_reported()) {
                    g_gp_seq.store(gp + 1U, memory_order::release);
                    g_qs_mask.reset();
                }
            } else {
                bool need_gp = false;
                arch::s390x::cpu::cpumask::for_each_online_cpu(
                    [&](u16 cpu) noexcept -> void {
                        auto* lc = arch::s390x::cpu::lowcore::get_for_cpu(cpu);
                        if (lc && arch::s390x::cpu::percpu::at(cpu).rcu_pending_count > 0U)
                            need_gp = true;
                    });
                if (need_gp) {
                    g_qs_mask.reset();
                    g_gp_seq.store(gp + 1U, memory_order::release);
                }
            }
            gp_lock_release();
        }

        const gp_seq_t done_gp = g_gp_seq.load(memory_order::acquire);
        if (!gp_in_progress(done_gp) && pcp.rcu_pending_cbs)
            drain_callbacks(pcp, done_gp);
    }

} // namespace zxfoundation::sync::rcu
