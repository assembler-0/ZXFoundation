/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/smp.cxxm
/// @brief SMP CPU bringup and IPI dispatch.

module arch.s390x.cpu.smp;
import arch.s390x.cpu.sync;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.clock;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.sigp;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.psw;
import arch.s390x.init.zxfl.protocol;
import arch.s390x.cpu.hotplug;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.ipi;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.hhdm;
import zxfoundation.sched.core;
import zxfoundation.sched.topology.core;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.irq.core;
import lib.error;
import std;

namespace arch::s390x::cpu::smp::detail {
    no_export std::atomic<u32> g_ap_online{0};
    no_export std::atomic<u32> g_sched_ready{0};
    no_export std::atomic<u32> g_aps_initialized{0};
    no_export u32 g_total_aps{0};
}

from_global(nomangle) {
    auto ap_entry() -> void;
}

declscope(C) {

    [[noreturn]] auto ap_startup() -> void {
        arch::s390x::cpu::ctlreg::ctl_set_bit<0>(37);
        arch::s390x::cpu::ctlreg::ctl_set_bit<0>(52);

        {
            const u64 cr3 = 0xFFFF0000ULL;
            const u64 cr4 = 0xFFFF0000ULL;
            arch::s390x::cpu::ctlreg::ctl_load<3, 3>(cr3);
            arch::s390x::cpu::ctlreg::ctl_load<4, 4>(cr4);

            u64 cr6 = 0;
            arch::s390x::cpu::ctlreg::ctl_store<6, 6>(cr6);
            cr6 |= 0x00000000FF000000ULL;
            arch::s390x::cpu::ctlreg::ctl_load<6, 6>(cr6);
        }

        discard_value arch::s390x::cpu::smp::detail::g_ap_online.fetch_add(1, std::memory_order::seq_cst);

        while (arch::s390x::cpu::smp::detail::g_sched_ready.load(std::memory_order::acquire) == 0)
            arch::s390x::cpu::sync::cpu_relax();

        const auto& pcp = arch::s390x::cpu::lowcore::this_cpu_percpu();
        zxfoundation::sched::sched_init_ap(pcp.cpu_id);

        discard_value arch::s390x::cpu::smp::detail::g_aps_initialized.fetch_add(1, std::memory_order::seq_cst);

        zxfoundation::sched::idle_enter();
    }

} // no_mangle

namespace arch::s390x::cpu::smp {

    using namespace detail;

    enum class ap_boot_state : u8 {
        allocated,
        lowcore_initialized,
        prefix_set,
        restarted,
        online,
        failed,
    };

    struct ap_boot_record {
        u16 cpu_id{};
        u16 cpu_addr{};
        u64 lowcore_phys{};
        u64 kernel_stack_base{};
        u64 async_stack_base{};
        u64 mcck_stack_base{};
        u64 nodat_stack_base{};
        ap_boot_state state{ap_boot_state::allocated};
        u32 sigp_status{};
    };

    no_export constexpr u64 TOD_UNITS_PER_MILLISECOND = 4'096'000ULL;
    no_export constexpr u64 AP_ONLINE_TIMEOUT_TOD = TOD_UNITS_PER_MILLISECOND * 30'000ULL;
    no_export constexpr u64 LOWCORE_PREFIX_ALIGNMENT = 8'192ULL;
    no_export constexpr u64 SIGP_PREFIX_MAX_PHYS = 1ULL << 31U;

    no_export std::array<ap_boot_record,zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_ap_boot_records{};

    no_export auto apply_cpu_evidence(
        arch::s390x::cpu::percpu::percpu_data& pcp,
        const arch::s390x::init::zxfl::cpu_info& ci
    ) noexcept -> void {
        pcp.cpu_type = ci.type;
        pcp.drawer_id = ci.drawer_id;
        pcp.core_id = ci.core_id;
        pcp.smt_id = ci.smt_id;
        pcp.topology_evidence = ci.topology_evidence;
        pcp.capacity = (ci.capacity == 0U) ? 100U : ci.capacity;
    }

    [[nodiscard]] no_export auto tod_elapsed(const u64 start) noexcept -> u64 {
        return arch::s390x::cpu::clock::stck() - start;
    }

    [[nodiscard]] no_export auto prefix_block_is_valid(const u64 phys) noexcept -> bool {
        return phys != 0 && phys < SIGP_PREFIX_MAX_PHYS &&
            ((phys & (LOWCORE_PREFIX_ALIGNMENT - 1U)) == 0);
    }

    /// @brief Free AP resources allocated via PMM after a mid-sequence failure.
    ///        Called only after txn.commit() so pages must be freed individually.
    no_export auto rollback_ap_resources(ap_boot_record& record) noexcept -> void {
        using namespace zxfoundation::memory::pmm;

        auto free_if = [](u64 phys, u32 order, purpose p) noexcept -> void {
            if (phys == 0) return;
            const zone z = (p == purpose::lowcore) ? zone::dma31 : zone::normal;
            folio_active f{phys, order, p, z};
            free(std::move(f));
        };

        free_if(record.lowcore_phys,        1, purpose::lowcore);
        free_if(record.kernel_stack_base,   2, purpose::stack);
        free_if(record.async_stack_base,    2, purpose::stack);
        free_if(record.mcck_stack_base,     2, purpose::stack);
        free_if(record.nodat_stack_base,    2, purpose::stack);

        record = {};
    }

    [[nodiscard]] no_export auto sigp_failure_error(const arch::s390x::cpu::sigp::sigp_result& result) noexcept -> lib::kernel_error {
        if (result.cc == arch::s390x::cpu::sigp::SIGP_CC_BUSY)
            return lib::kernel_error::from_arch(lib::arch_error::sigp_busy);
        if (result.cc == arch::s390x::cpu::sigp::SIGP_CC_NOT_OPERATIONAL)
            return lib::kernel_error::from_arch(lib::arch_error::sigp_not_operational);
        if (result.cc == arch::s390x::cpu::sigp::SIGP_CC_STATUS_STORED) {
            if (result.status & arch::s390x::cpu::sigp::SIGP_STATUS_INVALID_ORDER)
                return lib::kernel_error::from_arch(lib::arch_error::sigp_invalid_order);
            if (result.status & arch::s390x::cpu::sigp::SIGP_STATUS_INVALID_PARAMETER)
                return lib::kernel_error::from_memory(lib::memory_error::alignment_error);
            return lib::kernel_error::from_arch(lib::arch_error::cpu_not_available);
        }
        return lib::kernel_error::from_generic(lib::generic_error::internal_error);
    }

    [[nodiscard]] no_export auto reject_sigp_and_rollback(
        ap_boot_record& record,
        const u8 order,
        const arch::s390x::cpu::sigp::sigp_result& result
    ) noexcept -> lib::kernel_error {
        const auto err = sigp_failure_error(result);
        record.state = ap_boot_state::failed;
        record.sigp_status = result.status;
        zxfoundation::sys::printk::pnotice(
            "smp: cpu {} addr {} rejected by SIGP order {} cc {} status {}",
            record.cpu_id, record.cpu_addr, static_cast<u32>(order), result.cc, result.status);
        // Free all pages allocated for this AP (post-commit cleanup).
        rollback_ap_resources(record);
        return err;
    }

    [[nodiscard]] no_export auto wait_ap_online(const u32 target) noexcept -> bool {
        const u64 start = arch::s390x::cpu::clock::stck();
        while (g_ap_online.load(std::memory_order::seq_cst) < target) {
            if (tod_elapsed(start) >= AP_ONLINE_TIMEOUT_TOD)
                return false;
            arch::s390x::cpu::sync::cpu_relax();
        }
        return true;
    }
    
    no_export auto setup_bsp() noexcept -> void {
        discard_value zxfoundation::sys::irq::irq_register(IPI_IRQ_EMERGENCY, ipi::handle_ipi, nullptr, 0);
        discard_value zxfoundation::sys::irq::irq_register(IPI_IRQ_EXTCALL,   ipi::handle_ipi, nullptr, 0);
        ctlreg::ctl_set_bit<0>(37);
        ctlreg::ctl_set_bit<0>(52);

        hotplug::hotplug_init();
        hotplug::hotplug_mark_present(0U);
        hotplug::hotplug_mark_possible(0U);
    }

    no_export auto init_aps(const init::zxfl::protocol& proto) noexcept
        -> ::std::expected<u32, ::lib::kernel_error> {
        using namespace zxfoundation::memory::pmm;

        g_ap_online.store(0, std::memory_order::seq_cst);
        g_sched_ready.store(0, std::memory_order::release);
        g_aps_initialized.store(0, std::memory_order::seq_cst);
        g_total_aps = 0;
        g_ap_boot_records.fill({});

        if (proto.cpu_count > zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] {
            zxfoundation::sys::printk::pnotice(
                "smp: rejected cpu map count {} larger than maximum {}",
                proto.cpu_count, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS);
            return ::std::unexpected(::lib::kernel_error::from_arch(::lib::arch_error::invalid_cpu_id));
        }

        cpumask::cpu_set_possible(0);
        for (u32 i = 0; i < proto.cpu_count; ++i) {
            if (proto.cpu_map[i].cpu_addr == proto.bsp_cpu_addr) {
                auto& bsp_pcp = lowcore::this_cpu_percpu();
                apply_cpu_evidence(bsp_pcp, proto.cpu_map[i]);
                break;
            }
        }

        if (!(proto.flags & init::zxfl::ZXFL_FLAG_SMP) || proto.cpu_count <= 1)
            return 0U;

        const u64 ap_entry_phys =
            zxfoundation::memory::hhdm::virt_to_phys(reinterpret_cast<u64>(ap_entry));
        const u64 kernel_asce = lowcore::get_current()->kernel_asce;
        u64 cr0; ctlreg::ctl_store<0, 0>(cr0);

        u32 started = 0;
        u16 next_cpu_id = 1;

        g_sched_ready.store(1, std::memory_order::release);

        for (u32 i = 0; i < proto.cpu_count; ++i) {
            auto bail = [&](ap_boot_record& rec, const char* msg) -> void {
                rec.state = ap_boot_state::failed;
                rec.lowcore_phys = 0;
                rec.kernel_stack_base = 0;
                rec.async_stack_base = 0;
                rec.mcck_stack_base = 0;
                rec.nodat_stack_base = 0;
                rec.sigp_status = 0;
                zxfoundation::sys::printk::pnotice(
                    "smp: cpu {} addr {}: {}", rec.cpu_id, rec.cpu_addr, msg);
            };

            const auto& ci = proto.cpu_map[i];
            if (ci.cpu_addr == proto.bsp_cpu_addr) continue;
            if (ci.state != init::zxfl::ZXFL_CPU_STOPPED) continue;

            const u16 new_cpu_id = next_cpu_id++;
            if (new_cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] {
                zxfoundation::sys::printk::pnotice(
                    "smp: cpu addr {} skipped — no free cpu_id slot", ci.cpu_addr);
                --next_cpu_id;
                continue;
            }

            auto& record = g_ap_boot_records[new_cpu_id];
            record = ap_boot_record{
                .cpu_id = new_cpu_id,
                .cpu_addr = ci.cpu_addr,
            };

            cpumask::cpu_set_possible(new_cpu_id);
            hotplug::hotplug_mark_present(new_cpu_id);
            hotplug::hotplug_mark_possible(new_cpu_id);

            transaction txn;
            auto lc_f    = txn.alloc(1, zone::dma31, purpose::lowcore,    page_init::zeroed);
            if (!lc_f)   { bail(record, "lowcore allocation failed"); continue; }
            auto stk_f   = txn.alloc(2, zone::normal, purpose::stack,    page_init::zeroed);
            if (!stk_f)  { bail(record, "kernel stack allocation failed"); continue; }
            auto async_f = txn.alloc(2, zone::normal, purpose::stack,    page_init::zeroed);
            if (!async_f){ bail(record, "async stack allocation failed"); continue; }
            auto mcck_f  = txn.alloc(2, zone::normal, purpose::stack,    page_init::zeroed);
            if (!mcck_f) { bail(record, "mcck stack allocation failed"); continue; }
            auto nodat_f = txn.alloc(2, zone::normal, purpose::stack,    page_init::zeroed);
            if (!nodat_f){ bail(record, "nodat stack allocation failed"); continue; }

            const u64 lc_phys   = lc_f->phys_base;
            const u64 stk_top   = stk_f->phys_base   + (static_cast<u64>(PAGE_SIZE) << 2);
            const u64 async_top = async_f->phys_base  + (static_cast<u64>(PAGE_SIZE) << 2);
            const u64 mcck_top  = mcck_f->phys_base   + (static_cast<u64>(PAGE_SIZE) << 2);
            const u64 nodat_top = nodat_f->phys_base  + (static_cast<u64>(PAGE_SIZE) << 2);

            record.lowcore_phys = lc_phys;
            record.kernel_stack_base = stk_f->phys_base;
            record.async_stack_base = async_f->phys_base;
            record.mcck_stack_base = mcck_f->phys_base;
            record.nodat_stack_base = nodat_f->phys_base;

            if (!prefix_block_is_valid(lc_phys)) {
                bail(record, "lowcore prefix block misaligned or out of range");
                continue;
            }

            auto* lc = reinterpret_cast<lowcore::zx_lowcore*>(
                zxfoundation::memory::hhdm::phys_to_virt(lc_phys));

            lowcore::init_ap(*lc, lowcore::ap_init_params{
                .phys_lc       = lc_phys,
                .ap_entry_phys = ap_entry_phys,
                .kernel_asce   = kernel_asce,
                .ap_cr0        = cr0,
                .ap_stack_phys = stk_top,
                .cpu_id        = new_cpu_id,
                .cpu_addr      = ci.cpu_addr,
            });

            lc->kernel_stack = zxfoundation::memory::hhdm::phys_to_virt(stk_top);
            lc->async_stack  = zxfoundation::memory::hhdm::phys_to_virt(async_top);
            lc->mcck_stack   = zxfoundation::memory::hhdm::phys_to_virt(mcck_top);
            lc->nodat_stack  = zxfoundation::memory::hhdm::phys_to_virt(nodat_top);

            percpu::init_ap(lc->percpu);
            apply_cpu_evidence(lc->percpu, ci);
            lowcore::register_cpu_lowcore(new_cpu_id, lc_phys);
            record.state = ap_boot_state::lowcore_initialized;

            txn.commit();

            auto result = sigp::issue_bounded(ci.cpu_addr, sigp::SIGP_INITIAL_CPU_RESET, 0);
            if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED) {
                discard_value reject_sigp_and_rollback(record, sigp::SIGP_INITIAL_CPU_RESET, result);
                continue;
            }

            result = sigp::wait_cpu_stopped(ci.cpu_addr);
            if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED ||
                !(result.status & sigp::SIGP_STATUS_STOPPED) ||
                (result.status & sigp::SIGP_STATUS_CHECK_STOP)) {
                discard_value reject_sigp_and_rollback(record, sigp::SIGP_SENSE, result);
                continue;
            }

            result = sigp::issue_bounded(ci.cpu_addr, sigp::SIGP_SET_PREFIX,
                static_cast<u32>(lc_phys));
            if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED) {
                discard_value reject_sigp_and_rollback(record, sigp::SIGP_SET_PREFIX, result);
                continue;
            }
            record.state = ap_boot_state::prefix_set;

            sync::ssync();

            result = sigp::issue_bounded(ci.cpu_addr, sigp::SIGP_RESTART, 0);
            if (result.cc != sigp::SIGP_CC_ORDER_CODE_ACCEPTED) {
                discard_value reject_sigp_and_rollback(record, sigp::SIGP_RESTART, result);
                continue;
            }
            record.state = ap_boot_state::restarted;
            ++started;
        }

        if (started != 0) {
            if (!wait_ap_online(started)) {
                const u32 arrived =
                    g_ap_online.load(std::memory_order::seq_cst);
                zxfoundation::sys::printk::pnotice(
                    "smp: only {} of {} ap(s) entered startup before timeout",
                    arrived, started);
                // Clean up APs that never came online.
                for (u16 id = 1; id < next_cpu_id; ++id) {
                    if (g_ap_boot_records[id].state == ap_boot_state::restarted)
                        rollback_ap_resources(g_ap_boot_records[id]);
                }
                started = arrived;  // reflect what actually arrived
            }

            for (u16 id = 1; id < next_cpu_id; ++id) {
                if (g_ap_boot_records[id].state == ap_boot_state::restarted) {
                    g_ap_boot_records[id].state = ap_boot_state::online;
                }
            }

            zxfoundation::sys::printk::pinfo("smp: {} ap(s) started", started);
        } else {
            zxfoundation::sys::printk::pnotice("smp: no ap(s) detected/started");
        }

        g_total_aps = started;
        return started;
    }

    /// @brief Signal APs that the scheduler is ready.
    no_export auto ap_release_all() noexcept -> void {
        g_sched_ready.store(1, std::memory_order::release);
    }

    /// @brief BSP waits until all APs have finished sched_init_ap().
    no_export auto wait_aps_initialized() noexcept -> void {
        while (g_aps_initialized.load(std::memory_order::seq_cst) < g_total_aps)
            sync::cpu_relax();
    }

    auto launch(const init::zxfl::protocol& protocol) noexcept -> void {
        setup_bsp();
        discard_value init_aps(protocol);
        ap_release_all();
        wait_aps_initialized();
    }

} // namespace arch::s390x::cpu::smp
