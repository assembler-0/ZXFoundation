/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/softirq.cxx
/// @brief SCHED_SOFTIRQ dispatcher and balancing orchestration.

module zxfoundation.sched.softirq.core;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sys.syschk.core;
import zxfoundation.sched.balance.core;
import arch.s390x.cpu.percpu;
import lib.softirq.core;
import lib.error;
import lib.stl;

namespace zxfoundation::sched::softirq::detail {

    /// @brief Per-CPU balance request state indexed by CPU id.
    alignas(64) no_export std::array<balance_req, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_balance_reqs{};

    /// @brief Softirq entry point
    no_export auto softirq_entry(void* data) noexcept -> void {
        const u16 cpu = (data != nullptr)
            ? static_cast<u16>(reinterpret_cast<u64>(data) & 0xFFFFULL)
            : arch::s390x::cpu::percpu::this_cpu().cpu_id;
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] return;
        auto& req = g_balance_reqs[cpu];
        const u8 kind_u = req.kind.load(std::memory_order::acquire);
        const balance_kind kind = static_cast<balance_kind>(kind_u);
        discard_value lib::softirq_clear(lib::softirq_vec::sched);

        req.last_migration_ok.store(0U, std::memory_order::release);

        zxfoundation::sched::balance::balance_dispatch(cpu, kind, req);
        discard_value req.completion_count.fetch_add(1U, std::memory_order::release);
    }

} // namespace detail

namespace zxfoundation::sched::softirq {

    /// @brief Open the SCHED_SOFTIRQ vector with the framework.
    /// @return Success when the vector was registered.
    auto open_sched_softirq() noexcept -> std::expected<void, lib::kernel_error> {
        return lib::open_softirq(lib::softirq_vec::sched, detail::softirq_entry, nullptr);
    }

    /// @brief Raises SCHED_SOFTIRQ on a target CPU and records the balance kind.
    /// @param[in] cpu  Target CPU id.
    /// @param[in] kind Balance kind to request.
    auto raise_balance(u16 cpu, balance_kind kind) noexcept -> void {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] return;
        auto& req = detail::g_balance_reqs[cpu];
        req.kind.store(static_cast<u8>(kind), std::memory_order::release);
        discard_value req.request_count.fetch_add(1U, std::memory_order::relaxed);
        const u16 self = arch::s390x::cpu::percpu::this_cpu().cpu_id;
        if (cpu == self) {
            lib::raise_softirq(lib::softirq_vec::sched);
        } else {
            lib::raise_softirq_on(cpu, lib::softirq_vec::sched);
        }
    }

    /// @brief Convenience: raises SCHED_SOFTIRQ on the running CPU.
    auto raise_balance_local(balance_kind kind) noexcept -> void {
        const u16 cpu = arch::s390x::cpu::percpu::this_cpu().cpu_id;
        raise_balance(cpu, kind);
    }

    /// @brief Snapshot-read the per-CPU balance request for diagnostics.
    /// @return Const reference to the per-CPU balance_req.
    [[nodiscard]] auto balance_req_of(u16 cpu) noexcept -> const balance_req& {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] {
            zxfoundation::sys::syschk::syschk_fatal(
                lib::kernel_error::from_arch(lib::arch_error::invalid_cpu_id),
                "sched_softirq: balance_req_of out-of-range cpu={}",
                cpu);
        }
        return detail::g_balance_reqs[cpu];
    }

} // namespace zxfoundation::sched::softirq