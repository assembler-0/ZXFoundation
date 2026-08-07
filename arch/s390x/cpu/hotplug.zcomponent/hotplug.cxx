/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/hotplug.cxxm
/// @brief CPU hotplug state machine — online/offline transitions and notifier chain.

module arch.s390x.cpu.hotplug;
import arch.s390x.cpu.smp;
import arch.s390x.cpu.sigp;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.sync;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.ar;
import zxfoundation.base.config;
import zxfoundation.base.types;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm_types;
import zxfoundation.memory.hhdm;
import zxfoundation.sched.sched;
import zxfoundation.sched.sched_topology;
import zxfoundation.smp.smp;
import zxfoundation.sys.printk;
import lib.error;
import lib.static_vector;
import std;

namespace arch::s390x::cpu::hotplug::_internal {

    __internal__ std::atomic<u32> g_hotplug_initialized{0};

    __internal__ std::array<cpu_hotplug_state, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_cpu_states{};

    __internal__ ::lib::static_vector<hotplug_callback, HOTPLUG_NOTIFIER_MAX> g_notifiers{};

    [[nodiscard]] __internal__ constexpr auto is_initialized() noexcept -> bool {
        return g_hotplug_initialized.load(std::memory_order::acquire) != 0U;
    }

    __internal__ auto set_initialized() noexcept -> void {
        g_hotplug_initialized.store(1, std::memory_order::release);
    }

    __internal__ auto fire_notify(u16 cpu_id, cpu_hotplug_notify reason) noexcept -> void {
        for (auto& n : g_notifiers) {
            if (n) n(cpu_id, reason);
        }
    }

    [[nodiscard]] __internal__ auto sigp_stop_cpu(u16 cpu_addr) noexcept -> bool {
        auto r = arch::s390x::cpu::sigp::issue(cpu_addr,
                    arch::s390x::cpu::sigp::SIGP_STOP, 0);
        if (r.cc == arch::s390x::cpu::sigp::SIGP_CC_ORDER_CODE_ACCEPTED)
            return true;
        // retry once if busy
        if (r.cc == arch::s390x::cpu::sigp::SIGP_CC_BUSY) {
            arch::s390x::cpu::sync::cpu_relax();
            r = arch::s390x::cpu::sigp::issue(cpu_addr,
                    arch::s390x::cpu::sigp::SIGP_STOP, 0);
        }
        return r.cc == arch::s390x::cpu::sigp::SIGP_CC_ORDER_CODE_ACCEPTED;
    }

    __internal__ auto purge_alb_on_cpu(u64, void*) noexcept -> void {
        arch::s390x::cpu::ar::purge_alb();
    }

} // namespace _internal

namespace arch::s390x::cpu::hotplug {

    /// @brief Initialize the hotplug subsystem — mark BSP online.
    auto hotplug_init() noexcept -> void {
        for (auto & g_cpu_state : _internal::g_cpu_states) {
            g_cpu_state = cpu_hotplug_state::absent;
        }
        _internal::g_cpu_states[0] = cpu_hotplug_state::online;
        _internal::set_initialized();
    }

    /// @brief Register a hotplug notifier.
    /// @param[in] cb Callback invoked on state transitions.
    /// @return true if registered, false if the table is full.
    auto hotplug_register_notifier(hotplug_callback cb) noexcept -> bool {
        if (!cb) return false;
        if (!_internal::g_notifiers.emplace_back(cb)) return false;
        return true;
    }

    /// @brief Mark a CPU slot as present (firmware reports hardware exists).
    /// @param[in] cpu_id Logical CPU ID.
    auto hotplug_mark_present(u16 cpu_id) noexcept -> void {
        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) return;
        if (_internal::g_cpu_states[cpu_id] == cpu_hotplug_state::absent) {
            _internal::g_cpu_states[cpu_id] = cpu_hotplug_state::present;
            _internal::fire_notify(cpu_id, cpu_hotplug_notify::pre_setup);
        }
    }

    /// @brief Mark a CPU as possible (ready for online).
    /// @param[in] cpu_id Logical CPU ID.
    auto hotplug_mark_possible(u16 cpu_id) noexcept -> void {
        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) return;
        if (_internal::g_cpu_states[cpu_id] == cpu_hotplug_state::present ||
            _internal::g_cpu_states[cpu_id] == cpu_hotplug_state::offline) {
            _internal::g_cpu_states[cpu_id] = cpu_hotplug_state::possible;
        }
    }

    /// @brief Bring a CPU online.
    /// @param[in] cpu_id Logical CPU ID to online.
    /// @return Success or kernel_error describing why the transition failed.
    [[nodiscard]] auto cpu_up(u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error>
    {
        if (!_internal::is_initialized())
            return ::std::unexpected(::lib::kernel_error::from_generic(
                ::lib::generic_error::uninitialized));

        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));

        const auto current = _internal::g_cpu_states[cpu_id];
        if (current == cpu_hotplug_state::online)
            return {};

        if (current != cpu_hotplug_state::possible &&
            current != cpu_hotplug_state::offline)
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));

        _internal::fire_notify(cpu_id, cpu_hotplug_notify::pre_up);

        auto sched_result = zxfoundation::sched::sched_cpu_up(cpu_id);
        if (!sched_result) {
            _internal::g_cpu_states[cpu_id] = cpu_hotplug_state::failed;
            return ::std::unexpected(sched_result.error());
        }

        _internal::fire_notify(cpu_id, cpu_hotplug_notify::post_up);
        _internal::g_cpu_states[cpu_id] = cpu_hotplug_state::online;
        return {};
    }

    /// @brief Take a CPU offline.
    /// @param[in] cpu_id Logical CPU ID to offline.
    /// @return Success or kernel_error.
    /// @warning The caller must ensure all runnable strands are migrated
    ///          before calling this function.
    [[nodiscard]] auto cpu_down(u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error>
    {
        if (!_internal::is_initialized())
            return ::std::unexpected(::lib::kernel_error::from_generic(
                ::lib::generic_error::uninitialized));

        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));

        if (_internal::g_cpu_states[cpu_id] != cpu_hotplug_state::online)
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));

        if (cpu_id == 0)
            return ::std::unexpected(::lib::kernel_error::from_generic(
                ::lib::generic_error::invalid_arg));

        if (cpu_id == lowcore::this_cpu_percpu().cpu_id)
            return ::std::unexpected(::lib::kernel_error::from_generic(
                ::lib::generic_error::invalid_arg));

        _internal::fire_notify(cpu_id, cpu_hotplug_notify::pre_down);

        // Purge first while remote dispatch remains admitted. Closing admission
        // afterwards waits out every CSD accepted before the target is stopped.
        auto purge_result = zxfoundation::smp::smp_call_function_single_sync(
            cpu_id, _internal::purge_alb_on_cpu, nullptr, 0U);
        if (!purge_result) {
            _internal::g_cpu_states[cpu_id] = cpu_hotplug_state::failed;
            return std::unexpected(purge_result.error());
        }
        zxfoundation::smp::smp_csd_disable_cpu(cpu_id);
        cpumask::cpu_clear_online(cpu_id);

        auto* lc = lowcore::get_for_cpu(cpu_id);
        if (!lc) {
            zxfoundation::smp::smp_csd_enable_cpu(cpu_id);
            cpumask::cpu_set_online(cpu_id);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }
        const u16 cpu_addr = lc->percpu.cpu_addr;
        if (!_internal::sigp_stop_cpu(cpu_addr)) {
            zxfoundation::smp::smp_csd_enable_cpu(cpu_id);
            cpumask::cpu_set_online(cpu_id);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::sigp_busy));
        }

        auto sched_result = zxfoundation::sched::sched_cpu_down(cpu_id);
        if (!sched_result) {
            _internal::g_cpu_states[cpu_id] = cpu_hotplug_state::failed;
            return ::std::unexpected(sched_result.error());
        }

        zxfoundation::sched::topology::topology_offline_cpu(cpu_id);

        _internal::fire_notify(cpu_id, cpu_hotplug_notify::post_down);
        _internal::g_cpu_states[cpu_id] = cpu_hotplug_state::offline;
        return {};
    }

    /// @brief Query the current hotplug state of a CPU.
    [[nodiscard]] auto cpu_state(u16 cpu_id) noexcept -> cpu_hotplug_state {
        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
            return cpu_hotplug_state::absent;
        return _internal::g_cpu_states[cpu_id];
    }

    /// @brief Check if a CPU is online.
    [[nodiscard]] auto cpu_is_online(u16 cpu_id) noexcept -> bool {
        return cpu_state(cpu_id) == cpu_hotplug_state::online;
    }

    /// @brief Check if a CPU can be brought online.
    [[nodiscard]] auto cpu_is_possible(u16 cpu_id) noexcept -> bool {
        const auto s = cpu_state(cpu_id);
        return s == cpu_hotplug_state::possible || s == cpu_hotplug_state::offline;
    }

    /// @brief Check if hardware exists at this slot.
    [[nodiscard]] auto cpu_is_present(u16 cpu_id) noexcept -> bool {
        return cpu_state(cpu_id) != cpu_hotplug_state::absent;
    }

    /// @brief Resolve hotplug notify reason to a human-readable string.
    [[nodiscard]] auto hotplug_notify_str(cpu_hotplug_notify r) noexcept
        -> ::std::string_view
    {
        switch (r) {
            case cpu_hotplug_notify::pre_up:          return "pre_up";
            case cpu_hotplug_notify::post_up:         return "post_up";
            case cpu_hotplug_notify::pre_down:        return "pre_down";
            case cpu_hotplug_notify::post_down:       return "post_down";
            case cpu_hotplug_notify::pre_setup:       return "pre_setup";
            case cpu_hotplug_notify::post_teardown:   return "post_teardown";
        }
        return "unknown";
    }

} // namespace arch::s390x::cpu::hotplug
