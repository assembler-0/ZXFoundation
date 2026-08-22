/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/smp/smp.cxxm
/// @brief Cross-CPU function-call IPI subsystem — queue, completion, RCU free.

module zxfoundation.smp.csd.core;
import zxfoundation.smp.csd.types;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sync.rcu.core;
import zxfoundation.sys.printk.core;
import zxfoundation.sync.rcu.types;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.ipi;
import arch.s390x.cpu.sync;
import arch.s390x.cpu.cpumask;
import lib.cpumask;
import lib.error;
import std;

namespace zxfoundation::smp::csd::detail {

    /// @brief Per-CPU LIFO head pointer — module-static, indexed by cpu_id.
    alignas(64) std::array<std::atomic<call_single_data*>, base::config::CONFIG_ZX_MAX_CPUS> g_csd_head{};

    /// @brief Per-CPU statistics, indexed by cpu_id.
    alignas(64) std::array<csd_stats, base::config::CONFIG_ZX_MAX_CPUS> g_csd_stats{};

    /// @brief Per-CPU dispatch admission state.
    constexpr u32 CSD_DISPATCH_DISABLED = 1U << 31;
    constexpr u32 CSD_DISPATCH_COUNT_MASK = ~CSD_DISPATCH_DISABLED;
    alignas(64) std::array<std::atomic<u32>, base::config::CONFIG_ZX_MAX_CPUS> g_csd_dispatch_state{};

    [[nodiscard]] auto dispatch_lease_try_acquire(u16 cpu) noexcept -> bool {
        auto& state = g_csd_dispatch_state[cpu];
        u32 observed = state.load(std::memory_order::acquire);
        while ((observed & CSD_DISPATCH_DISABLED) == 0U) {
            if ((observed & CSD_DISPATCH_COUNT_MASK) == CSD_DISPATCH_COUNT_MASK)
                return false;
            if (state.compare_exchange_weak(
                    observed, observed + 1U,
                    std::memory_order::acq_rel,
                    std::memory_order::acquire))
                return true;
        }
        return false;
    }

    auto dispatch_lease_release(u16 cpu) noexcept -> void {
        discard_value g_csd_dispatch_state[cpu].fetch_sub(1U, std::memory_order::release);
    }

    /// @brief Atomically push a csd onto a CPU's LIFO head (Treiber stack).
    [[nodiscard]] auto queue_push(u16 cpu, call_single_data* csd) noexcept -> bool {
        auto& head = g_csd_head[cpu];
        call_single_data* expected = head.load(std::memory_order::relaxed);
        csd->next.store(expected, std::memory_order::relaxed);
        while (!head.compare_exchange_weak(
            expected, csd,
            std::memory_order::release,
            std::memory_order::relaxed))
        {
            csd->next.store(expected, std::memory_order::relaxed);
        }
        ++g_csd_stats[cpu].enqueue_count;
        return expected == nullptr;
    }

    /// @brief Atomically pop the entire csd LIFO for the running CPU.
    /// @return The previous head (list to walk) or nullptr when empty.
    [[nodiscard]] auto queue_drain_all() noexcept -> call_single_data* {
        const u16 cpu = arch::s390x::cpu::lowcore::this_cpu_percpu().cpu_id;
        auto& head = g_csd_head[cpu];
        return head.exchange(nullptr, std::memory_order::acq_rel);
    }

} // namespace zxfoundation::smp::detail

namespace zxfoundation::smp::csd {

    /// @brief Initialise a csd before enqueue.
    /// @param[out] csd  CSD node to initialise.
    /// @param[in]  func Callback invoked on the target CPU.
    /// @param[in]  data Opaque @c data pointer passed back to @c func.
    /// @param[in]  info Caller-supplied fixed-width word passed to @c func.
    /// @param[in]  flags One of @c CSD_F_SYNC / @c CSD_F_ASYNC, may
    ///             be ORed with @c CSD_F_RCU_FREE (async-only).
    auto init(call_single_data& csd, csd_fn func, void* data,
                  u64 info, u32 flags) noexcept -> void {
        csd.next.store(nullptr, std::memory_order::relaxed);
        csd.func = func;
        csd.data = data;
        csd.info = info;
        csd.done_atm = 0;
        const u32 locked_flags = flags | CSD_F_LOCKED;
        csd.flags.store(locked_flags, std::memory_order::release);
    }

    /// @brief Wait until the target CPU has completed dispatch of @c csd.
    /// @param[in] csd CSD node with @c CSD_F_SYNC set by the producer.
    auto lock_wait(const call_single_data& csd) noexcept -> void {
        while ((csd.flags.load(std::memory_order::acquire) & CSD_F_DONE) == 0U) {
            arch::s390x::cpu::sync::cpu_relax();
        }
    }

    /// @brief Test whether @c csd has completed dispatch.
    /// @return True when @c CSD_F_DONE is observed on @c csd.flags.
    [[nodiscard]] auto is_done(const call_single_data& csd) noexcept -> bool {
        return (csd.flags.load(std::memory_order::acquire) & CSD_F_DONE) != 0U;
    }

    /// @brief Register an RCU-freeing callback on a csd flagged with
    ///        @c CSD_F_RCU_FREE.
    /// @param[out] csd CSD node flagged @c CSD_F_RCU_FREE.
    /// @param[in]  fn  Callback invoked by @c call_rcu after a grace period.
    auto set_rcu_free(call_single_data& csd,
                          void (*fn)(sync::rcu::rcu_head*) noexcept) noexcept -> void {
        csd.done_atm = reinterpret_cast<u64>(reinterpret_cast<void*>(fn));
    }

    /// @brief Drain all pending CSD nodes for the running CPU.
    auto drain_cpu() noexcept -> void {
        call_single_data* node = detail::queue_drain_all();
        while (node) {
            call_single_data* const next = node->next.load(std::memory_order::acquire);
            const u32 flags = node->flags.load(std::memory_order::relaxed);
            csd_fn invoked_fn = node->func;
            u64 invoked_info = node->info;
            void* invoked_data = node->data;
            if (invoked_fn) {
                invoked_fn(invoked_info, invoked_data);
            }
            if (flags & CSD_F_RCU_FREE) {
                auto const free_fn =
                    reinterpret_cast<void(*)(sync::rcu::rcu_head*) noexcept>(
                        node->done_atm);
                auto* rh =
                    reinterpret_cast<sync::rcu::rcu_head*>(&node->next);
                rh->next = nullptr;
                rh->func = free_fn ? free_fn
                    : +[](sync::rcu::rcu_head* h) noexcept -> void { discard_value h; };
                sync::rcu::call_rcu(rh, rh->func);
            } else if (flags & CSD_F_SYNC) {
                discard_value node->flags.fetch_or(CSD_F_DONE, std::memory_order::release);
            } else {
                discard_value node->flags.fetch_and(
                    ~(CSD_F_LOCKED | CSD_F_QUEUED),
                    std::memory_order::release);
            }
            if (flags & CSD_F_DISPATCH_LEASE)
                detail::dispatch_lease_release(
                    arch::s390x::cpu::lowcore::this_cpu_percpu().cpu_id);
            ++detail::g_csd_stats[arch::s390x::cpu::lowcore::this_cpu_percpu().cpu_id]
                 .dispatch_count;
            node = next;
        }
    }

    /// @brief Enqueue a pre-initialised csd onto a target CPU.
    /// @param[in] cpu  Target CPU id.
    /// @param[in] csd  CSD node, fully initialised via @c csd_init.
    /// @note Suppresses self-IPI when @c cpu == this_cpu (caller-local pass).
    auto call_function_single_async(u16 cpu, call_single_data* csd) noexcept
        -> std::expected<void, lib::kernel_error>
    {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS || csd == nullptr) [[unlikely]] {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        const u32 flags = csd->flags.load(std::memory_order::relaxed);
        const bool is_sync  = (flags & CSD_F_SYNC)  != 0U;
        const bool is_async = (flags & CSD_F_ASYNC) != 0U;
        const bool is_rcu_free = (flags & CSD_F_RCU_FREE) != 0U;
        if (is_sync == is_async) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        if (is_rcu_free && !is_async) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        const u16 self = arch::s390x::cpu::lowcore::this_cpu_percpu().cpu_id;
        ++detail::g_csd_stats[self].csd_call_count;

        if (cpu != self) {
            if (!arch::s390x::cpu::cpumask::cpu_online(cpu) ||
                !detail::dispatch_lease_try_acquire(cpu)) {
                return std::unexpected(
                    lib::kernel_error::from_arch(lib::arch_error::cpu_not_available));
            }
            discard_value csd->flags.fetch_or(
                CSD_F_DISPATCH_LEASE, std::memory_order::release);
        }

        const bool need_ipi = detail::queue_push(cpu, csd);
        discard_value csd->flags.fetch_or(CSD_F_QUEUED, std::memory_order::release);

        if (cpu == self) {
            discard_value drain_cpu();
            return {};
        }
        if (need_ipi) {
            arch::s390x::cpu::ipi::send_call_function(cpu);
        }
        return {};
    }

    /// @brief Prevent new remote CSD enqueues to @p cpu and wait until all
    ///        already accepted callbacks have completed on the target.
    /// @note CPU teardown keeps the target runnable until this wait completes.
    auto disable_cpu(u16 cpu) noexcept -> void {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] return;
        auto& state = detail::g_csd_dispatch_state[cpu];
        const u32 old = state.fetch_or(
            detail::CSD_DISPATCH_DISABLED, std::memory_order::acq_rel);
        while ((state.load(std::memory_order::acquire) & detail::CSD_DISPATCH_COUNT_MASK) != 0U)
            arch::s390x::cpu::sync::cpu_relax();
        discard_value old;
    }

    /// @brief Allow remote CSD enqueues to @p cpu after it has been initialized.
    auto enable_cpu(u16 cpu) noexcept -> void {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] return;
        detail::g_csd_dispatch_state[cpu].store(0U, std::memory_order::release);
    }

    /// @brief Synchronous single-CPU function-call IPI.
    /// @param[in] cpu   Target CPU id.
    /// @param[in] func  Callback invoked on the target CPU.
    /// @param[in] data  Opaque data passed to @c func.
    /// @param[in] info  Fixed-width word passed to @c func.
    /// @return Success when the target CPU completed dispatch; @c invalid_arg
    ///         on bad CPU id or null callback.
    auto call_function_single_sync(u16 cpu, csd_fn func, void* data,
                                       u64 info) noexcept
        -> std::expected<void, lib::kernel_error>
    {
        if (func == nullptr || cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        call_single_data csd;
        init(csd, func, data, info, CSD_F_SYNC);
        const u16 self = arch::s390x::cpu::lowcore::this_cpu_percpu().cpu_id;
        if (cpu == self) {
            func(info, data);
            return {};
        }
        auto r = call_function_single_async(cpu, &csd);
        if (!r) return r;
        lock_wait(csd);
        return {};
    }

    /// @brief Fan-out a synchronous callback to many CPUs at once.
    /// @param[in] mask          CPU mask of target CPUs.
    /// @param[in] func          Callback invoked on each target CPU.
    /// @param[in] data          Opaque data shared by all target invocations.
    /// @param[in] info          Fixed-width word shared by all target invocations.
    /// @param[out] csd_pool     Caller-owned descriptors, one per remote CPU.
    /// @param[in] pool_capacity Number of descriptors available in @p csd_pool.
    /// @return Number of CPUs whose callbacks completed.
    /// @note All remote callbacks are queued before any completion is waited
    ///       on.  This avoids serialising the IPI round trips.
    auto call_function_many(const ::lib::cpumask& mask, csd_fn func,
                                void* data, u64 info,
                                call_single_data* csd_pool,
                                u32 pool_capacity) noexcept
        -> std::expected<u32, lib::kernel_error>
    {
        if (func == nullptr || csd_pool == nullptr || pool_capacity == 0U) [[unlikely]] {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        const u16 self = arch::s390x::cpu::lowcore::this_cpu_percpu().cpu_id;
        ++detail::g_csd_stats[self].csd_call_many_count;

        u32 remote_targets = 0U;
        mask.for_each([&](u16 cpu) noexcept -> void {
            if (cpu != self) ++remote_targets;
        });
        if (remote_targets > pool_capacity) [[unlikely]] {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::out_of_memory));
        }

        u32 queued = 0U;
        bool local_invoked = false;
        lib::kernel_error dispatch_error{};
        bool failed = false;
        u32 idx = 0U;
        mask.for_each([&](u16 cpu) noexcept -> void {
            if (failed) return;
            if (cpu == self) {
                func(info, data);
                local_invoked = true;
                return;
            }

            auto& csd = csd_pool[idx++];
            init(csd, func, data, info, CSD_F_SYNC);
            auto result = call_function_single_async(cpu, &csd);
            if (!result) {
                if (!failed) dispatch_error = result.error();
                failed = true;
                return;
            }
            ++queued;
        });

        for (u32 i = 0U; i < idx; ++i)
            if (csd_pool[i].flags.load(std::memory_order::acquire) & CSD_F_QUEUED)
            lock_wait(csd_pool[i]);

        if (failed) return std::unexpected(dispatch_error);
        return queued + (local_invoked ? 1U : 0U);
    }

    /// @brief Test whether the running CPU has any pending csd nodes.
    /// @return True when the running CPU's CSD queue is non-empty.
    [[nodiscard]] auto pending() noexcept -> bool {
        const u16 cpu = arch::s390x::cpu::lowcore::this_cpu_percpu().cpu_id;
        return detail::g_csd_head[cpu].load(std::memory_order::acquire) != nullptr;
    }

    /// @brief Read the per-CPU CSD statistics block for diagnostics.
    [[nodiscard]] auto stats_of(u16 cpu) noexcept -> csd_stats {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] return {};
        return detail::g_csd_stats[cpu];
    }

} // namespace zxfoundation::smp
