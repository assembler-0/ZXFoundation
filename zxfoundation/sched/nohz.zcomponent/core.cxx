/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/nohz.zcomponent/core.cxx
/// @brief Per-CPU tickless (nohz) idle state machine.

module zxfoundation.sched.nohz.core;
import zxfoundation.base.types;
import zxfoundation.sched.core;
import zxfoundation.sched.softirq.core;
import zxfoundation.sys.timer;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.clock;

namespace zxfoundation::sched::nohz {
    
    /// @details The hooks operate on the current CPU's tick timer and raise
    ///          SCHED_SOFTIRQ so the balancer can re-evaluate on wake.
    auto do_prepare_idle() noexcept -> void {}
    
    auto do_tick_stop() noexcept -> void {
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto& t = cpu_timer(pcp.cpu_id);
        if (t.armed) discard_value zxfoundation::sys::timer::timer_cancel(t);
    }
    
    auto do_tick_start() noexcept -> void {
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto& t = cpu_timer(pcp.cpu_id);
        if (!t.armed) {
            t.deadline_tod = arch::s390x::cpu::clock::stck() + zxfoundation::sys::timer::TICK_INTERVAL_TOD;
            discard_value zxfoundation::sys::timer::timer_add(t);
        }
    }
    
    auto do_enter_balance() noexcept -> void {
        zxfoundation::sched::softirq::raise_balance_local(
            zxfoundation::sched::softirq::balance_kind::nohz_idle);
    }

    /// @brief Constant nohz ops table installed on every CPU.
    const zxfoundation::sched::nohz::nohz_ops the_ops{
        .prepare_idle  = &do_prepare_idle,
        .enter_balance = &do_enter_balance,
        .tick_stop     = &do_tick_stop,
        .tick_start    = &do_tick_start,
        .context       = nullptr
    };
    
    /// @brief Enter nohz idle on the current CPU.
    auto idle_enter(u32& flags, const nohz_ops& ops) noexcept -> void {
        if (ops.prepare_idle != nullptr) ops.prepare_idle();
        const bool already = (flags & static_cast<u32>(nohz_flag::tick_stopped)) != 0U;
        if (already) return;
        if (ops.tick_stop != nullptr) ops.tick_stop();
        flags |= static_cast<u32>(nohz_flag::tick_stopped);
    }

    /// @brief Exit nohz idle on the current CPU (called on any wakeup).
    /// @param[in,out] flags Per-CPU nohz_flag word.
    /// @param[in]     ops        Operational hooks.
    /// @note Re‑opens the periodic tick timer so scheduler_tick() fires again.
    auto idle_exit(u32& flags, const nohz_ops& ops) noexcept -> void {
        if (ops.enter_balance != nullptr) ops.enter_balance();
        if (ops.tick_start != nullptr) ops.tick_start();
        flags &= ~static_cast<u32>(nohz_flag::tick_stopped);
    }

    /// @brief Test whether the nohz-idle kick is pending on the current CPU.
    [[nodiscard]] auto balance_pending(u32 flags) noexcept -> bool {
        return (flags & static_cast<u32>(nohz_flag::nr_balance_pending)) != 0U;
    }

    /// @brief Clear the nohz-balance pending nohz_flag (committed by the balancer
    ///        on the first pass of the softirq).
    auto clear_balance(u32& flags) noexcept -> void {
        flags &= ~static_cast<u32>(nohz_flag::nr_balance_pending);
    }

} // namespace zxfoundation::sched::nohz