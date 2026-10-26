/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/smp.zcomponent/core.cxx
/// @brief Serialized ownership of s390x CPU lifecycle transitions.

module arch.s390x.cpu.smp.core;
import arch.s390x.cpu.sigp;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.features;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.sync;
import arch.s390x.cpu.clock;
import arch.s390x.cpu.ipi;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.hhdm;
import zxfoundation.sched.core;
import zxfoundation.sched.topology.core;
import zxfoundation.smp.csd.core;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.base.config;
import zxfoundation.base.types;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.irq.core;
import zxfoundation.init.storage.core;
import zxfoundation.sys.zinit.core;
import lib.error;
import lib.static_vector;
import lib.stl;
import zxfoundation.sync.lockable;

#define __zxmacro_want_zinit
#include macro_extensions

from_global(nomangle) {
    auto ap_entry() -> void;
}

declscope(C) {
    [[noreturn]] auto ap_startup() -> void;
}

namespace arch::s390x::cpu::smp::detail {

    using namespace zxfoundation::base::config;
    using lock_type = zxfoundation::sync::qspinlock::qspinlock<zxfoundation::sync::lock_rank::smp_doorbell>;

    no_export lock_type g_lock{};
    no_export lock_type g_transition_lock{};
    no_export lock_type g_bringup_lock{};
    no_export bool g_initialized{false};
    no_export std::array<lifecycle_state, CONFIG_ZX_MAX_CPUS> g_states{};
    no_export std::array<cpu_descriptor, CONFIG_ZX_MAX_CPUS> g_descriptors{};
    no_export std::array<lifecycle_callback, LIFECYCLE_NOTIFIER_MAX> g_callbacks{};
    no_export u32 g_callback_count{0U};
    no_export std::atomic<u32> g_ap_online{0U};
    no_export std::atomic<u32> g_sched_ready{0U};
    no_export std::atomic<u32> g_aps_initialized{0U};
    no_export std::array<std::atomic<u32>, CONFIG_ZX_MAX_CPUS> g_ap_ack{};
    no_export std::array<std::atomic<u32>, CONFIG_ZX_MAX_CPUS> g_ap_cancel{};

    struct ap_boot_record {
        u16 cpu_id{};
        u16 cpu_addr{};
        u64 lowcore_phys{};
        u64 kernel_stack_base{};
        u64 async_stack_base{};
        u64 mcck_stack_base{};
        u64 nodat_stack_base{};
    };

    no_export std::array<ap_boot_record, CONFIG_ZX_MAX_CPUS> g_ap_records{};
    no_export u32 g_total_aps{0U};
    no_export bool g_bringup_complete{false};

    no_export constexpr u64 TOD_UNITS_PER_MILLISECOND = 4'096'000ULL;
    no_export constexpr u64 AP_ONLINE_TIMEOUT_TOD = TOD_UNITS_PER_MILLISECOND * 30'000ULL;
    no_export constexpr u64 LOWCORE_PREFIX_ALIGNMENT = 8192ULL;
    no_export constexpr u64 SIGP_PREFIX_MAX_PHYS = 1ULL << 31U;

    no_export auto apply_cpu_evidence(
        percpu::percpu_data& pcp,
        const init::zxfl::cpu_info& ci
    ) noexcept -> void {
        pcp.cpu_type = ci.type;
        pcp.drawer_id = ci.drawer_id;
        pcp.core_id = ci.core_id;
        pcp.smt_id = ci.smt_id;
        pcp.topology_evidence = ci.topology_evidence;
        pcp.capacity = (ci.capacity == 0U) ? 100U : ci.capacity;
    }

    [[nodiscard]] no_export auto prefix_block_is_valid(const u64 phys) noexcept -> bool {
        return phys != 0ULL && phys < SIGP_PREFIX_MAX_PHYS &&
            (phys & (LOWCORE_PREFIX_ALIGNMENT - 1ULL)) == 0ULL;
    }

    no_export auto rollback(ap_boot_record& record) noexcept -> void {
        using namespace zxfoundation::memory::pmm;
        lowcore::unregister_cpu_lowcore(record.cpu_id, record.lowcore_phys);
        discard_value smp::unbind_lowcore(record.cpu_id, record.lowcore_phys);

        auto free_if = [](const u64 phys, const u32 order, const purpose p) noexcept -> void {
            if (phys == 0ULL) return;
            const zone z = (p == purpose::lowcore) ? zone::dma31 : zone::normal;
            folio_active f{phys, order, p, z};
            free(std::move(f));
        };
        free_if(record.lowcore_phys, 1U, purpose::lowcore);
        free_if(record.kernel_stack_base, 2U, purpose::stack);
        free_if(record.async_stack_base, 2U, purpose::stack);
        free_if(record.mcck_stack_base, 2U, purpose::stack);
        free_if(record.nodat_stack_base, 2U, purpose::stack);
        record = {};
    }

    no_export auto release_record(ap_boot_record& record) noexcept -> void {
        rollback(record);
    }

    [[nodiscard]] auto invalid_cpu(u16 cpu_id) noexcept -> bool {
        return cpu_id >= CONFIG_ZX_MAX_CPUS;
    }

    auto notify(u16 cpu_id, const notify_reason reason) noexcept -> void {
        for (u32 i = 0U; i < g_callback_count; ++i) {
            if (g_callbacks[i]) g_callbacks[i](cpu_id, reason);
        }
    }

    [[nodiscard]] auto sigp_error(const sigp::sigp_result& result) noexcept
        -> lib::kernel_error {
        if (result.cc == sigp::SIGP_CC_BUSY)
            return lib::kernel_error::from_arch(lib::arch_error::sigp_busy);
        if (result.cc == sigp::SIGP_CC_NOT_OPERATIONAL)
            return lib::kernel_error::from_arch(lib::arch_error::sigp_not_operational);
        if (result.status & sigp::SIGP_STATUS_INVALID_PARAMETER)
            return lib::kernel_error::from_memory(lib::memory_error::alignment_error);
        if (result.status & sigp::SIGP_STATUS_INVALID_ORDER)
            return lib::kernel_error::from_arch(lib::arch_error::sigp_invalid_order);
        return lib::kernel_error::from_arch(lib::arch_error::cpu_not_available);
    }

    [[nodiscard]] auto lock() noexcept -> ::irqflags {
        return g_lock.lock_irqsave();
    }

    auto unlock(const ::irqflags flags) noexcept -> void {
        g_lock.unlock_irqrestore(flags);
    }

    [[nodiscard]] auto valid_transition(const lifecycle_state from,
                                         const lifecycle_state to) noexcept -> bool {
        if (from == lifecycle_state::possible && to == lifecycle_state::starting) return true;
        if (from == lifecycle_state::starting && to == lifecycle_state::online) return true;
        if (from == lifecycle_state::online && to == lifecycle_state::quiescing) return true;
        if (from == lifecycle_state::quiescing && to == lifecycle_state::stopping) return true;
        if (from == lifecycle_state::stopping && to == lifecycle_state::offline) return true;
        if (from == lifecycle_state::offline && to == lifecycle_state::starting) return true;
        if (to == lifecycle_state::failed) return true;
        return false;
    }

    [[nodiscard]] auto valid_prefix(const u64 phys) noexcept -> bool {
        return phys == 0ULL || prefix_block_is_valid(phys);
    }

    [[nodiscard]] auto signal_order_allowed(const u8 order) noexcept -> bool {
        return order == sigp::SIGP_EXTERNAL_CALL ||
            order == sigp::SIGP_EMERGENCY_SIGNAL ||
            order == sigp::SIGP_STOP_AND_STORE_STATUS;
    }

    [[nodiscard]] auto hw_initial_cpu_reset(const u16 cpu_addr) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        const auto result = sigp::issue_bounded(cpu_addr, sigp::SIGP_INITIAL_CPU_RESET, 0U);
        if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED)
            return ::std::unexpected(sigp_error(result));
        return {};
    }

    [[nodiscard]] auto hw_wait_stopped(const u16 cpu_addr) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        const auto result = sigp::wait_cpu_stopped(cpu_addr);
        if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED ||
            !(result.status & (sigp::SIGP_STATUS_STOPPED | sigp::SIGP_STATUS_CHECK_STOP)))
            return ::std::unexpected(sigp_error(result));
        return {};
    }

    [[nodiscard]] auto hw_set_prefix(const u16 cpu_addr, const u64 lowcore_phys) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (!valid_prefix(lowcore_phys) || lowcore_phys >= SIGP_PREFIX_MAX_PHYS)
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::alignment_error));
        const auto result = sigp::issue_bounded(cpu_addr, sigp::SIGP_SET_PREFIX,
            static_cast<u32>(lowcore_phys));
        if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED)
            return ::std::unexpected(sigp_error(result));
        return {};
    }

    [[nodiscard]] auto hw_restart(const u16 cpu_addr) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        const auto result = sigp::issue_bounded(cpu_addr, sigp::SIGP_RESTART, 0U);
        if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED)
            return ::std::unexpected(sigp_error(result));
        return {};
    }

    [[nodiscard]] auto hw_stop(const u16 cpu_addr) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        auto result = sigp::issue_bounded(cpu_addr, sigp::SIGP_STOP, 0U);
        if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED)
            return ::std::unexpected(sigp_error(result));
        result = sigp::wait_cpu_stopped(cpu_addr);
        if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED ||
            !(result.status & (sigp::SIGP_STATUS_STOPPED | sigp::SIGP_STATUS_CHECK_STOP)))
            return ::std::unexpected(sigp_error(result));
        return {};
    }

    no_export auto stop_and_reclaim(ap_boot_record& record) noexcept -> bool {
        if (record.lowcore_phys == 0ULL)
            return true;
        const auto stopped = hw_stop(record.cpu_addr);
        if (!stopped)
            return false;
        const auto reset_prefix = hw_set_prefix(record.cpu_addr, 0ULL);
        if (!reset_prefix)
            return false;
        rollback(record);
        return true;
    }

    auto publish_failure(const u16 cpu_id) noexcept -> void {
        {
            const auto flags = lock();
            if (cpu_id < CONFIG_ZX_MAX_CPUS)
                g_states[cpu_id] = lifecycle_state::failed;
            unlock(flags);
        }
        notify(cpu_id, notify_reason::failed);
    }

    [[nodiscard]] auto online_impl(const u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        const auto flags = lock();
        if (g_states[cpu_id] == lifecycle_state::online) {
            unlock(flags);
            return {};
        }
        if (!valid_transition(g_states[cpu_id], lifecycle_state::online)) {
            unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }
        unlock(flags);
        const auto attach = zxfoundation::sched::scheduler_cpu_attach(cpu_id);
        if (!attach) {
            publish_failure(cpu_id);
            return attach;
        }
        const auto publish_flags = lock();
        if (g_states[cpu_id] != lifecycle_state::starting) {
            unlock(publish_flags);
            discard_value zxfoundation::sched::scheduler_cpu_detach(cpu_id);
            publish_failure(cpu_id);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }
        g_states[cpu_id] = lifecycle_state::online;
        zxfoundation::smp::csd::enable_cpu(cpu_id);
        cpumask::cpu_set_online(cpu_id);
        unlock(publish_flags);
        zxfoundation::sched::topology::refresh_cpu_record(cpu_id, 0U, true);
        zxfoundation::sched::topology::topology_online_cpu(cpu_id);
        notify(cpu_id, notify_reason::post_online);
        return {};
    }

} // namespace arch::s390x::cpu::smp::detail

declscope(C) {
    [[noreturn]] auto ap_startup() -> void {
        using namespace arch::s390x::cpu;
        features::enable_hardware_facilities();
        while (smp::detail::g_sched_ready.load(std::memory_order::acquire) == 0U)
            sync::cpu_relax();

        const auto& pcp = percpu::this_cpu();
        const u16 cpu_id = pcp.cpu_id;
        if (cpu_id < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS &&
            smp::detail::g_ap_cancel[cpu_id].load(std::memory_order::acquire) != 0U)
            psw::disabled_wait();

        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
            psw::disabled_wait();
        zxfoundation::sched::sched_init_ap(cpu_id);
        const auto result = smp::acknowledge_startup(cpu_id);
        if (!result) {
            discard_value zxfoundation::sched::scheduler_cpu_detach(cpu_id);
            if (cpu_id < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
                smp::detail::g_ap_cancel[cpu_id].store(1U, std::memory_order::release);
            psw::disabled_wait();
        }
        smp::detail::g_ap_ack[cpu_id].store(1U, std::memory_order::release);
        discard_value smp::detail::g_ap_online.fetch_add(1U, std::memory_order::seq_cst);
        discard_value smp::detail::g_aps_initialized.fetch_add(1U, std::memory_order::seq_cst);
        zxfoundation::sched::idle_enter();
    }
}

namespace arch::s390x::cpu::smp {

    auto zi_smp_launch(const zxfoundation::sys::zinit::zi_desc&) noexcept
        -> zxfoundation::sys::zinit::zi_result
    {
        const init::zxfl::protocol& protocol = zxfoundation::init::storage::get_protocol();
        const auto initialized = initialize();
        if (!initialized) return std::unexpected(initialized.error());
        // Reset bring-up state here — single-threaded, on the graph, before
        // any AP can exist. bringup() below is then a pure state machine.
        detail::g_ap_online.store(0U, std::memory_order::seq_cst);
        detail::g_sched_ready.store(0U, std::memory_order::release);
        detail::g_aps_initialized.store(0U, std::memory_order::seq_cst);
        detail::g_ap_records.fill({});
        detail::g_total_aps = 0U;
        for (u16 id = 0U; id < detail::CONFIG_ZX_MAX_CPUS; ++id) {
            detail::g_ap_ack[id].store(0U, std::memory_order::relaxed);
            detail::g_ap_cancel[id].store(0U, std::memory_order::relaxed);
        }
        discard_value zxfoundation::sys::irq::irq_register(
            IPI_IRQ_EMERGENCY, ipi::handle_ipi, nullptr, 0);
        discard_value zxfoundation::sys::irq::irq_register(
            IPI_IRQ_EXTCALL, ipi::handle_ipi, nullptr, 0);

        const auto result = smp::bringup(protocol);
        if (!result) {
            zxfoundation::sys::printk::pnotice(
                "smp: bring-up failed: {}", result.error());
            return std::unexpected(result.error());
        }
        zxfoundation::sys::printk::pinfo(
            "smp: {} ap(s) online", *result);
        arch::s390x::cpu::irq::irq_enable();
        return {};
    }
    __zinit_dep__(smp, p00, zi_smp_launch, fatal, "zi_sched_init")

    [[nodiscard]] auto lifecycle_state_str(const lifecycle_state state) noexcept
        -> ::std::string_view {
        switch (state) {
            case lifecycle_state::absent: return "absent";
            case lifecycle_state::present: return "present";
            case lifecycle_state::possible: return "possible";
            case lifecycle_state::starting: return "starting";
            case lifecycle_state::online: return "online";
            case lifecycle_state::quiescing: return "quiescing";
            case lifecycle_state::stopping: return "stopping";
            case lifecycle_state::offline: return "offline";
            case lifecycle_state::failed: return "failed";
        }
        return "unknown";
    }

    [[nodiscard]] auto initialize() noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        const auto flags = detail::lock();
        if (!detail::g_initialized) {
            detail::g_states.fill(lifecycle_state::absent);
            detail::g_descriptors.fill({});
            detail::g_callback_count = 0U;
            detail::g_initialized = true;
            detail::g_bringup_complete = false;
        }
        const auto& pcp = percpu::this_cpu();
        if (detail::g_states[0] == lifecycle_state::online) {
            detail::unlock(flags);
            return {};
        }
        detail::g_descriptors[0] = cpu_descriptor{
            .logical_id = 0U,
            .cpu_addr = pcp.cpu_addr,
            .lowcore_phys = lowcore::get_current_phys(),
            .generation = detail::g_descriptors[0].generation + 1U,
        };
        detail::g_states[0] = lifecycle_state::possible;
        cpumask::cpu_set_possible(0U);
        detail::unlock(flags);
        detail::notify(0U, notify_reason::post_setup);
        return {};
    }

    [[nodiscard]] auto register_cpu(const cpu_descriptor& descriptor) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (detail::invalid_cpu(descriptor.logical_id))
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));
        const auto flags = detail::lock();
        if (!detail::g_initialized) {
            detail::unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_generic(
                ::lib::generic_error::uninitialized));
        }
        auto& state = detail::g_states[descriptor.logical_id];
        const bool was_absent = state == lifecycle_state::absent;
        for (u16 id = 0U; id < detail::CONFIG_ZX_MAX_CPUS; ++id) {
            if (id != descriptor.logical_id &&
                detail::g_states[id] != lifecycle_state::absent &&
                detail::g_descriptors[id].cpu_addr == descriptor.cpu_addr) {
                detail::unlock(flags);
                return ::std::unexpected(::lib::kernel_error::from_generic(
                    ::lib::generic_error::invalid_arg));
            }
        }
        if (state != lifecycle_state::absent &&
            detail::g_descriptors[descriptor.logical_id].cpu_addr != descriptor.cpu_addr) {
            detail::unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_generic(
                ::lib::generic_error::invalid_arg));
        }
        auto stored = descriptor;
        stored.generation = detail::g_descriptors[descriptor.logical_id].generation + 1U;
        detail::g_descriptors[descriptor.logical_id] = stored;
        if (was_absent) {
            state = lifecycle_state::present;
        }
        cpumask::cpu_set_possible(descriptor.logical_id);
        detail::unlock(flags);
        if (was_absent)
            detail::notify(descriptor.logical_id, notify_reason::pre_setup);
        return {};
    }

    [[nodiscard]] auto bind_lowcore(const u16 cpu_id, const u64 lowcore_phys) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (detail::invalid_cpu(cpu_id))
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));
        if (!detail::prefix_block_is_valid(lowcore_phys))
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::alignment_error));
        const auto flags = detail::lock();
        if (!detail::g_initialized || detail::g_states[cpu_id] == lifecycle_state::absent) {
            detail::unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }
        for (u16 id = 0U; id < detail::CONFIG_ZX_MAX_CPUS; ++id) {
            if (id != cpu_id && detail::g_descriptors[id].lowcore_phys == lowcore_phys &&
                detail::g_states[id] != lifecycle_state::absent) {
                detail::unlock(flags);
                return ::std::unexpected(::lib::kernel_error::from_generic(
                    ::lib::generic_error::invalid_arg));
            }
        }
        detail::g_descriptors[cpu_id].lowcore_phys = lowcore_phys;
        detail::unlock(flags);
        return {};
    }

    [[nodiscard]] auto unbind_lowcore(const u16 cpu_id, const u64 lowcore_phys) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (detail::invalid_cpu(cpu_id))
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));

        const auto flags = detail::lock();
        if (!detail::g_initialized || detail::g_states[cpu_id] == lifecycle_state::absent) {
            detail::unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }

        auto& descriptor = detail::g_descriptors[cpu_id];
        if (descriptor.lowcore_phys != lowcore_phys) {
            detail::unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_generic(
                ::lib::generic_error::invalid_arg));
        }
        descriptor.lowcore_phys = 0ULL;
        detail::unlock(flags);
        return {};
    }

    [[nodiscard]] auto mark_possible(const u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (detail::invalid_cpu(cpu_id))
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));
        const auto flags = detail::lock();
        const auto state = detail::g_states[cpu_id];
        if (state != lifecycle_state::present && state != lifecycle_state::offline &&
            state != lifecycle_state::possible) {
            detail::unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }
        detail::g_states[cpu_id] = lifecycle_state::possible;
        cpumask::cpu_set_possible(cpu_id);
        detail::unlock(flags);
        return {};
    }

    [[nodiscard]] auto begin_start(const u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (detail::invalid_cpu(cpu_id))
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));
        const auto flags = detail::lock();
        if (!detail::valid_transition(detail::g_states[cpu_id], lifecycle_state::starting)) {
            detail::unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }
        detail::g_states[cpu_id] = lifecycle_state::starting;
        detail::unlock(flags);
        detail::notify(cpu_id, notify_reason::pre_online);
        return {};
    }

    [[nodiscard]] auto fail_start(const u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (detail::invalid_cpu(cpu_id))
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));
        const auto flags = detail::lock();
        auto& state = detail::g_states[cpu_id];
        if (state == lifecycle_state::failed) {
            detail::unlock(flags);
            return {};
        }
        if (state != lifecycle_state::starting && state != lifecycle_state::stopping) {
            detail::unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }
        state = lifecycle_state::failed;
        detail::unlock(flags);
        detail::notify(cpu_id, notify_reason::failed);
        return {};
    }

    [[nodiscard]] auto abort_start(const u16 cpu_id, const u16 cpu_addr) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (detail::invalid_cpu(cpu_id))
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));

        zxfoundation::sync::irq_lock_guard transaction{detail::g_transition_lock};
        {
            const auto flags = detail::lock();
            auto& state = detail::g_states[cpu_id];
            if (state == lifecycle_state::online) {
                detail::unlock(flags);
                return ::std::unexpected(::lib::kernel_error::from_arch(
                    ::lib::arch_error::cpu_not_available));
            }
            if (state != lifecycle_state::starting && state != lifecycle_state::failed) {
                detail::unlock(flags);
                return ::std::unexpected(::lib::kernel_error::from_arch(
                    ::lib::arch_error::cpu_not_available));
            }
            if (state == lifecycle_state::starting)
                state = lifecycle_state::stopping;
            detail::unlock(flags);
        }

        const auto stopped = detail::hw_stop(cpu_addr);
        if (!stopped) {
            detail::publish_failure(cpu_id);
            return std::unexpected(stopped.error());
        }

        const auto detach = zxfoundation::sched::scheduler_cpu_detach(cpu_id);
        zxfoundation::smp::csd::disable_cpu(cpu_id);
        cpumask::cpu_clear_online(cpu_id);
        zxfoundation::sched::topology::topology_offline_cpu(cpu_id);
        if (!detach) {
            detail::publish_failure(cpu_id);
            return detach;
        }
        const auto reset_prefix = detail::hw_set_prefix(cpu_addr, 0ULL);
        if (!reset_prefix) {
            detail::publish_failure(cpu_id);
            return reset_prefix;
        }
        detail::publish_failure(cpu_id);
        return {};
    }

    [[nodiscard]] auto acknowledge_startup(const u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        return online(cpu_id);
    }

    [[nodiscard]] auto online(const u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (detail::invalid_cpu(cpu_id))
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));
        zxfoundation::sync::irq_lock_guard transaction{detail::g_transition_lock};
        return detail::online_impl(cpu_id);
    }

    /// @brief Reap self-stopped CPUs
    auto reap_stopped() noexcept -> void {
        for (u32 i = 0; i < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++i) {
            const auto flags = detail::lock();
            const bool reap = detail::g_states[i] == lifecycle_state::stopping;
            detail::unlock(flags);
            if (!reap) continue;
            zxfoundation::sched::topology::topology_offline_cpu(i);
            const auto f2 = detail::lock();
            detail::g_states[i] = lifecycle_state::offline;
            detail::unlock(f2);
            detail::notify(i, notify_reason::post_offline);
            detail::release_record(detail::g_ap_records[i]);
        }
    }

    [[nodiscard]] auto offline(const u16 cpu_id) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (detail::invalid_cpu(cpu_id) || cpu_id == 0U || cpu_id == percpu::this_cpu().cpu_id)
            return ::std::unexpected(::lib::kernel_error::from_generic(
                ::lib::generic_error::invalid_arg));
        zxfoundation::sync::irq_lock_guard transaction{detail::g_transition_lock};
        const auto flags = detail::lock();
        const auto current_state = detail::g_states[cpu_id];
        if (current_state != lifecycle_state::online &&
            current_state != lifecycle_state::stopping) {
            detail::unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }
        const bool already_stopped = current_state == lifecycle_state::stopping;
        if (!already_stopped) {
            detail::g_states[cpu_id] = lifecycle_state::quiescing;
            zxfoundation::smp::csd::disable_cpu(cpu_id);
            cpumask::cpu_clear_online(cpu_id);
            detail::g_states[cpu_id] = lifecycle_state::stopping;
        }
        const u16 cpu_addr = detail::g_descriptors[cpu_id].cpu_addr;
        detail::unlock(flags);
        if (!already_stopped)
            detail::notify(cpu_id, notify_reason::pre_offline);
        const auto result = already_stopped ? detail::hw_wait_stopped(cpu_addr)
                                            : detail::hw_stop(cpu_addr);
        if (!result) {
            reap_stopped();
            detail::publish_failure(cpu_id);
            return result;
        }
        const auto detach = zxfoundation::sched::scheduler_cpu_detach(cpu_id);
        if (!detach) {
            detail::publish_failure(cpu_id);
            return detach;
        }
        const auto reset_prefix = detail::hw_set_prefix(cpu_addr, 0ULL);
        if (!reset_prefix) {
            detail::publish_failure(cpu_id);
            return reset_prefix;
        }
        zxfoundation::sched::topology::topology_offline_cpu(cpu_id);
        const auto final_flags = detail::lock();
        detail::g_states[cpu_id] = lifecycle_state::offline;
        detail::unlock(final_flags);
        detail::notify(cpu_id, notify_reason::post_offline);
        detail::release_record(detail::g_ap_records[cpu_id]);
        return {};
    }

    auto request_stop_current() noexcept -> void {
        const u16 cpu_id = percpu::this_cpu().cpu_id;
        const auto flags = detail::lock();
        if (cpu_id < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS &&
            detail::g_states[cpu_id] == lifecycle_state::online) {
            detail::g_states[cpu_id] = lifecycle_state::stopping;
            zxfoundation::smp::csd::disable_cpu(cpu_id);
            cpumask::cpu_clear_online(cpu_id);
        }
        detail::unlock(flags);
        // TODO: Self-stop contract: a CPU cannot dismantle its own scheduler
        // context from an interrupt handler. It halts in `stopping` with
        // its resources quarantined; reap_stopped() performs the remote
        // teardown.
        psw::disabled_wait();
    }

    [[nodiscard]] auto register_notifier(const lifecycle_callback callback) noexcept -> bool {
        if (!callback) return false;
        const auto flags = detail::lock();
        if (detail::g_callback_count >= LIFECYCLE_NOTIFIER_MAX) {
            detail::unlock(flags);
            return false;
        }
        detail::g_callbacks[detail::g_callback_count++] = callback;
        detail::unlock(flags);
        return true;
    }

    [[nodiscard]] auto state(const u16 cpu_id) noexcept -> lifecycle_state {
        if (detail::invalid_cpu(cpu_id)) return lifecycle_state::absent;
        const auto flags = detail::lock();
        const auto result = detail::g_states[cpu_id];
        detail::unlock(flags);
        return result;
    }

    [[nodiscard]] auto is_online(const u16 cpu_id) noexcept -> bool {
        return state(cpu_id) == lifecycle_state::online;
    }

    [[nodiscard]] auto signal(const u16 cpu_id, const u8 order, const u32 parameter) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (detail::invalid_cpu(cpu_id))
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::invalid_cpu_id));
        if (!detail::signal_order_allowed(order))
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::sigp_invalid_order));
        const auto flags = detail::lock();
        if (!detail::g_initialized || detail::g_states[cpu_id] == lifecycle_state::absent) {
            detail::unlock(flags);
            return ::std::unexpected(::lib::kernel_error::from_arch(
                ::lib::arch_error::cpu_not_available));
        }
        const u16 cpu_addr = detail::g_descriptors[cpu_id].cpu_addr;
        detail::unlock(flags);
        const auto result = sigp::issue_bounded(cpu_addr, order, parameter);
        if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED)
            return ::std::unexpected(detail::sigp_error(result));
        return {};
    }

    [[nodiscard]] auto bringup(const init::zxfl::protocol& protocol) noexcept
        -> ::std::expected<u32, ::lib::kernel_error> {
        using namespace detail;
        using namespace zxfoundation::memory::pmm;

        zxfoundation::sync::irq_try_guard bringup_serialization{detail::g_bringup_lock};
        if (!static_cast<bool>(bringup_serialization))
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::busy));

        if (!detail::g_initialized) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::uninitialized));
        }
        {
            const auto flags = detail::lock();
            if (detail::g_bringup_complete) {
                const u32 online_aps = detail::g_total_aps;
                detail::unlock(flags);
                return online_aps;
            }
            detail::unlock(flags);
        }
        const auto bsp_start = smp::begin_start(0U);
        if (!bsp_start && state(0U) != lifecycle_state::online)
            return std::unexpected(bsp_start.error());
        const auto bsp_online = online(0U);
        if (!bsp_online) return std::unexpected(bsp_online.error());

        // Bring-up counters are reset by zi_smp_launch on the init graph —
        // bringup() never mutates init state; it is a pure state machine
        // driven from a clean slate.
        if (protocol.cpu_count > CONFIG_ZX_MAX_CPUS)
            return std::unexpected(lib::kernel_error::from_arch(
                lib::arch_error::invalid_cpu_id));

        for (u32 i = 0U; i < protocol.cpu_count; ++i) {
            if (protocol.cpu_map[i].cpu_addr == protocol.bsp_cpu_addr) {
                apply_cpu_evidence(percpu::this_cpu(), protocol.cpu_map[i]);
                break;
            }
        }
        if (!(protocol.flags & init::zxfl::ZXFL_FLAG_SMP) || protocol.cpu_count <= 1U)
        {
            const auto flags = detail::lock();
            detail::g_bringup_complete = true;
            detail::unlock(flags);
            return 0U;
        }

        const u64 ap_entry_phys = zxfoundation::memory::hhdm::virt_to_phys(
            reinterpret_cast<u64>(ap_entry));
        const u64 kernel_asce = lowcore::get_current()->get<"kernel_asce">();
        u64 cr0{};
        ctlreg::crread<0, 0>(cr0);
        g_sched_ready.store(1U, std::memory_order::release);

        u32 started = 0U;
        u16 next_cpu_id = 1U;
        for (u32 i = 0U; i < protocol.cpu_count; ++i) {
            const auto& ci = protocol.cpu_map[i];
            if (ci.cpu_addr == protocol.bsp_cpu_addr || ci.state != init::zxfl::ZXFL_CPU_STOPPED)
                continue;
            if (next_cpu_id >= CONFIG_ZX_MAX_CPUS) break;

            const u16 cpu_id = next_cpu_id++;
            auto& record = g_ap_records[cpu_id];
            record.cpu_id = cpu_id;
            record.cpu_addr = ci.cpu_addr;
            const auto registered = register_cpu(cpu_descriptor{
                .logical_id = cpu_id, .cpu_addr = ci.cpu_addr});
            if (!registered || !mark_possible(cpu_id) || !begin_start(cpu_id)) {
                discard_value fail_start(cpu_id);
                continue;
            }

            transaction txn;
            auto lc_f = txn.alloc(1U, zone::dma31, purpose::lowcore, page_init::zeroed);
            auto stk_f = txn.alloc(2U, zone::normal, purpose::stack, page_init::zeroed);
            auto async_f = txn.alloc(2U, zone::normal, purpose::stack, page_init::zeroed);
            auto mcck_f = txn.alloc(2U, zone::normal, purpose::stack, page_init::zeroed);
            auto nodat_f = txn.alloc(2U, zone::normal, purpose::stack, page_init::zeroed);
            if (!lc_f || !stk_f || !async_f || !mcck_f || !nodat_f) {
                discard_value fail_start(cpu_id);
                continue;
            }

            record.lowcore_phys = lc_f->phys_base;
            record.kernel_stack_base = stk_f->phys_base;
            record.async_stack_base = async_f->phys_base;
            record.mcck_stack_base = mcck_f->phys_base;
            record.nodat_stack_base = nodat_f->phys_base;
            if (!prefix_block_is_valid(record.lowcore_phys)) {
                rollback(record);
                discard_value fail_start(cpu_id);
                continue;
            }

            auto* lc = reinterpret_cast<lowcore::zlowcore*>(
                zxfoundation::memory::hhdm::phys_to_virt(record.lowcore_phys));
            const u64 stack_top = record.kernel_stack_base + (static_cast<u64>(PAGE_SIZE) << 2U);
            const u64 async_top = record.async_stack_base + (static_cast<u64>(PAGE_SIZE) << 2U);
            const u64 mcck_top = record.mcck_stack_base + (static_cast<u64>(PAGE_SIZE) << 2U);
            const u64 nodat_top = record.nodat_stack_base + (static_cast<u64>(PAGE_SIZE) << 2U);
            lowcore::init_ap(*lc, lowcore::ap_init_params{
                .phys_lc = record.lowcore_phys, .ap_entry_phys = ap_entry_phys,
                .kernel_asce = kernel_asce, .ap_cr0 = cr0, .ap_stack_phys = stack_top,
                .cpu_id = cpu_id, .cpu_addr = ci.cpu_addr});
            lc->set<"kernel_stack">(zxfoundation::memory::hhdm::phys_to_virt(stack_top));
            lc->set<"async_stack">(zxfoundation::memory::hhdm::phys_to_virt(async_top));
            lc->set<"mcck_stack">(zxfoundation::memory::hhdm::phys_to_virt(mcck_top));
            lc->set<"nodat_stack">(zxfoundation::memory::hhdm::phys_to_virt(nodat_top));
            percpu::init_ap(percpu::at(cpu_id));
            apply_cpu_evidence(percpu::at(cpu_id), ci);
            lowcore::register_cpu_lowcore(cpu_id, record.lowcore_phys);
            const auto bound = bind_lowcore(cpu_id, record.lowcore_phys);
            if (!bound) {
                rollback(record);
                discard_value fail_start(cpu_id);
                continue;
            }
            txn.commit();

            const auto reset = detail::hw_initial_cpu_reset(ci.cpu_addr);
            const auto stopped = reset ? detail::hw_wait_stopped(ci.cpu_addr)
                                       : std::expected<void, lib::kernel_error>(std::unexpected(reset.error()));
            const auto prefixed = stopped ? detail::hw_set_prefix(ci.cpu_addr, record.lowcore_phys)
                                          : std::expected<void, lib::kernel_error>(std::unexpected(stopped.error()));
            const auto restarted = prefixed ? detail::hw_restart(ci.cpu_addr)
                                             : std::expected<void, lib::kernel_error>(std::unexpected(prefixed.error()));
            if (!restarted) {
                if (!stop_and_reclaim(record)) {
                    publish_failure(cpu_id);
                    continue;
                }
                discard_value fail_start(cpu_id);
                continue;
            }
            ++started;
        }

        const u64 deadline = clock::stck() + AP_ONLINE_TIMEOUT_TOD;
        while (g_ap_online.load(std::memory_order::acquire) < started && clock::stck() < deadline)
            sync::cpu_relax();
        for (u16 id = 1U; id < next_cpu_id; ++id) {
            if (g_ap_ack[id].load(std::memory_order::acquire) != 0U || is_online(id)) continue;
            g_ap_cancel[id].store(1U, std::memory_order::release);
            const auto record = g_ap_records[id];
            const auto aborted = abort_start(id, record.cpu_addr);
            if (aborted && state(id) != lifecycle_state::online) {
                if (!detail::stop_and_reclaim(g_ap_records[id]))
                    detail::publish_failure(id);
            }
        }
        const u32 total_aps = g_ap_online.load(std::memory_order::acquire);
        {
            const auto flags = detail::lock();
            g_total_aps = total_aps;
            g_bringup_complete = true;
            detail::unlock(flags);
        }
        return total_aps;
    }

} // namespace arch::s390x::cpu::smp
