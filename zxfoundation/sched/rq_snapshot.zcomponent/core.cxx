/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/rq_snapshot.zcomponent/core.cxx
/// @brief Remote run-queue snapshot taken via the CSD function-call IPI.

module zxfoundation.sched.rq_snapshot.core;
import zxfoundation.sched.core;
import zxfoundation.sched.rq.types;
import zxfoundation.base.types;
import zxfoundation.base.config;
import lib.error;
import lib.stl;

namespace zxfoundation::sched::rq_snapshot {

    static std::atomic<u16> g_steal_epoch{0};

    /// @brief Publish the run-queue fields used by remote balancers.
    /// @pre The caller owns @p rq.lock, or is publishing during CPU setup.
    auto publish(sched::rq::run_queue& rq) noexcept -> void {
        rq.balance_min_vruntime.store(rq.min_vruntime, std::memory_order::release);
        rq.balance_avg_vruntime_acc.store(rq.avg_vruntime_acc, std::memory_order::release);
        rq.balance_avg_weight_acc.store(rq.avg_weight_acc, std::memory_order::release);
        rq.balance_cfs_load_avg.store(rq.cfs_avg.load_avg, std::memory_order::release);
        rq.balance_cfs_util_avg.store(rq.cfs_avg.util_avg, std::memory_order::release);
        rq.balance_cfs_runnable_avg.store(rq.cfs_avg.runnable_avg, std::memory_order::release);
        rq.balance_nr_running.store(rq.nr_running, std::memory_order::release);
        rq.balance_cpu_capacity_curr.store(rq.cpu_capacity_curr, std::memory_order::release);
        rq.balance_online.store(rq.online.load(std::memory_order::relaxed), std::memory_order::release);
    }

    /// @brief Collect a lockless advisory snapshot of @c cpu's run queue.
    /// @param[in]  cpu Target CPU id (may equal this_cpu — handled inline).
    /// @param[out] out Snapshot filled by the target CPU.
    /// @return Success, or @c invalid_arg on bad CPU id.
    [[nodiscard]] auto collect(u16 cpu, rq_snapshot& out) noexcept
        -> std::expected<void, lib::kernel_error>
    {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        auto& rq = cpu_rq(cpu);
        out.target_cpu = cpu;
        out.min_vruntime = rq.balance_min_vruntime.load(std::memory_order::acquire);
        out.avg_vruntime_acc = rq.balance_avg_vruntime_acc.load(std::memory_order::acquire);
        out.avg_weight_acc = rq.balance_avg_weight_acc.load(std::memory_order::acquire);
        out.cfs_load_avg = rq.balance_cfs_load_avg.load(std::memory_order::acquire);
        out.cfs_util_avg = rq.balance_cfs_util_avg.load(std::memory_order::acquire);
        out.cfs_runnable_avg = rq.balance_cfs_runnable_avg.load(std::memory_order::acquire);
        out.nr_running = rq.balance_nr_running.load(std::memory_order::acquire);
        out.cpu_capacity_curr = rq.balance_cpu_capacity_curr.load(std::memory_order::acquire);
        out.online = rq.balance_online.load(std::memory_order::acquire);
        out.attached_asce = rq.balance_attached_asce.load(std::memory_order::acquire);
        out.attached_domain_id = rq.balance_attached_domain_id.load(std::memory_order::acquire);
        out.attached_dgp_state = rq.balance_attached_dgp_state.load(std::memory_order::acquire);
        return {};
    }

    /// @brief The CPU id owning the current idle-balance round
    [[nodiscard]] auto steal_epoch_slot() noexcept -> u16 {
        return g_steal_epoch.load(std::memory_order::relaxed);
    }

    /// @brief Hand the idle-balance round to the CPU after @p cpu.
    auto steal_epoch_advance(u16 cpu) noexcept -> void {
        g_steal_epoch.store(
            static_cast<u16>((cpu + 1U)
                % zxfoundation::base::config::CONFIG_ZX_MAX_CPUS),
            std::memory_order::relaxed);
    }

} // namespace zxfoundation::sched::rq_snapshot
