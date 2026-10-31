/// SPDX-License-Identifier: Apache-2.0
/// @file lib/softirq.zcomponent/core.cxx
/// @brief Generic softirq framework

module lib.softirq.core;
import lib.error;
import lib.format;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sys.irq.core;
import zxfoundation.sys.irq.consts;
import zxfoundation.sys.syschk.core;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.ipi;
import arch.s390x.cpu.cpumask;
import lib.stl;

namespace lib::detail {

    using namespace lib;

    /// @brief Global vector table shared by all CPUs.
    alignas(64) static std::array<softirq_entry, NR_SOFTIRQS> g_vector_table{};

    /// @brief Per-CPU softirq execution state indexed by logical CPU id.
    alignas(64) static std::array<softirq_state, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_per_cpu_state{};

    /// @brief Compute the per-CPU softirq state for the running CPU.
    [[nodiscard]] inline auto current_state() noexcept -> softirq_state& {
        const u16 cpu = arch::s390x::cpu::percpu::this_cpu().cpu_id;
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] {
            zxfoundation::sys::syschk::syschk_fatal(
                lib::kernel_error::from_arch(lib::arch_error::invalid_cpu_id),
                "softirq: per-cpu state lookup on out-of-range cpu={}",
                cpu);
        }
        return g_per_cpu_state[cpu];
    }

    inline auto kick_deferred_dispatch() noexcept -> void {
        zxfoundation::sys::irq::deferred_set(zxfoundation::sys::irq::DEFERRED_IRQ_WORK_BIT);
    }

} // namespace detail

namespace lib {

    /// @brief Register a handler for a softirq vector.
    /// @param[in] v      Softirq vector to register.
    /// @param[in] action Callback invoked by @c do_softirq when @c v is pending.
    /// @param[in] data   Opaque caller context passed back to @c action.
    /// @return Success when the slot was free, or @c kernel_error::already_locked
    ///         when the slot was already registered.
    [[nodiscard]] auto open_softirq(
        softirq_vec v,
        softirq_fn action,
        void* data
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (static_cast<u32>(v) >= NR_SOFTIRQS || action == nullptr) [[unlikely]] {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        auto& entry = detail::g_vector_table[static_cast<u32>(v)];
        if (entry.action != nullptr) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::internal_error));
        }
        entry.data = data;
        entry.action = action;
        // Bump the generation counter so diagnostics can observe registration.
        discard_value entry.generation.fetch_add(1U, std::memory_order::release);
        return {};
    }

    /// @brief Mark a softirq vector as pending on the current CPU.
    /// @param[in] v Vector to raise.
    auto raise_softirq(softirq_vec v) noexcept -> void {
        if (static_cast<u32>(v) >= NR_SOFTIRQS) [[unlikely]] return;
        auto& state = detail::current_state();
        const u32 bit = softirq_bit(v);
        const u32 old = state.pending.fetch_or(bit, std::memory_order::release);
        if ((old & bit) == 0U) {
            state.raise_count += 1U;
        }
        detail::kick_deferred_dispatch();
    }

    /// @brief Mark a softirq vector as pending on a *remote* CPU.
    /// @param[in] cpu Target CPU id (logical, not absolute).
    /// @param[in] v   Softirq vector to raise.
    auto raise_softirq_on(u16 cpu, softirq_vec v) noexcept -> void {
        if (static_cast<u32>(v) >= NR_SOFTIRQS) [[unlikely]] return;
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] return;
        if (!arch::s390x::cpu::cpumask::cpu_online(cpu)) [[unlikely]] return;
        auto& state = detail::g_per_cpu_state[cpu];
        const u32 bit = softirq_bit(v);
        const u32 old = state.pending.fetch_or(bit, std::memory_order::release);
        if ((old & bit) == 0U) {
            state.raise_count += 1U;
        }
        arch::s390x::cpu::ipi::send_irq_work(cpu);
    }

    /// @brief Test whether any softirq is pending on the current CPU.
    /// @return True when at least one vector is pending and dispatchable.
    [[nodiscard]] auto softirq_pending() noexcept -> bool {
        const auto& state = detail::current_state();
        return state.pending.load(std::memory_order::acquire) != 0U;
    }

    /// @brief Drain all pending softirq vectors on the current CPU.
    auto do_softirq() noexcept -> void {
        auto& state = detail::current_state();
        if (state.nest >= 10U) [[unlikely]] return;

        ++state.nest;
        ++state.dispatch_count;

        for (;;) {
            const u32 pending = state.pending.exchange(
                0U, std::memory_order::acq_rel);
            if (pending == 0U) break;
            state.processing = pending;

            // Drain in vector-index order so a higher-priority vector is
            // always serviced before a lower one.
            for (u32 idx = 0U; idx < NR_SOFTIRQS; ++idx) {
                if ((pending & (1U << idx)) == 0U) continue;
                auto& entry = detail::g_vector_table[idx];
                auto* action = entry.action;
                if (action != nullptr) {
                    action(entry.data);
                }
                state.processing &= ~(1U << idx);
            }
        }

        --state.nest;
    }

    /// @brief Clear a pending softirq vector without dispatching it.
    /// @param[in] v Vector to clear.
    /// @return True when a pending bit was cleared; false when no work was pending.
    [[nodiscard]] auto softirq_clear(softirq_vec v) noexcept -> bool {
        if (static_cast<u32>(v) >= NR_SOFTIRQS) [[unlikely]] return false;
        auto& state = detail::current_state();
        const u32 bit = softirq_bit(v);
        const u32 old = state.pending.fetch_and(~bit, std::memory_order::acq_rel);
        return (old & bit) != 0U;
    }

    /// @brief Test whether a particular softirq vector is pending on this CPU.
    /// @param[in] v Vector to test.
    [[nodiscard]] auto softirq_test(softirq_vec v) noexcept -> bool {
        if (static_cast<u32>(v) >= NR_SOFTIRQS) [[unlikely]] return false;
        const auto& state = detail::current_state();
        const u32 bit = softirq_bit(v);
        return (state.pending.load(std::memory_order::acquire) & bit) != 0U;
    }

    /// @brief Read this CPU's softirq dispatch statistics (for diagnostics).
    /// @return A pair (raise_count, dispatch_count).
    [[nodiscard]] auto softirq_stats() noexcept -> std::pair<u64, u64> {
        const auto& state = detail::current_state();
        return { state.raise_count, state.dispatch_count };
    }

    /// @brief Return the global vector-table entry for a softirq vector.
    /// @param[in] v Vector to inspect.
    /// @return Const reference to the registered entry (action may be nullptr
    ///         when the slot was never opened).
    [[nodiscard]] auto softirq_entry_for(softirq_vec v) noexcept -> const softirq_entry& {
        if (static_cast<u32>(v) >= NR_SOFTIRQS) [[unlikely]] {
            zxfoundation::sys::syschk::syschk_fatal(
                lib::kernel_error::from_generic(lib::generic_error::out_of_range),
                "softirq: vector {} out of range [0,{})",
                static_cast<u32>(v), NR_SOFTIRQS);
        }
        return detail::g_vector_table[static_cast<u32>(v)];
    }

} // namespace lib
