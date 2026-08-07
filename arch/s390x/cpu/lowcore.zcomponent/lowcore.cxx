/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/lowcore.cxxm
/// @brief Lowcore (prefix area) management: access, initialization, and AP setup.

module arch.s390x.cpu.lowcore;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.types;
import zxfoundation.memory.hhdm;
import zxfoundation.base.types;
import zxfoundation.base.config;
import arch.s390x.init.zxfl.protocol;
import std;


/// @brief Physical addresses of each CPU's lowcore page, indexed by logical CPU ID.
__internal__ std::array<std::atomic<u64>, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> __percpu_lowcore_phys{};
alignas(16) __internal__ std::byte s_boot_async_stack[arch::s390x::cpu::processor::ASYNC_STACK_SIZE]{};
alignas(16) __internal__ std::byte s_boot_mcck_stack[arch::s390x::cpu::processor::MCCK_STACK_SIZE]{};
alignas(16) __internal__ std::byte s_boot_nodat_stack[arch::s390x::cpu::processor::NODAT_STACK_SIZE]{};
alignas(16) __internal__ std::byte s_boot_restart_stack[arch::s390x::cpu::processor::STACK_SIZE]{};

extern "C" {
    auto trap_pgm_entry()     -> void;
    auto trap_ext_entry()     -> void;
    auto trap_io_entry()      -> void;
    auto trap_svc_entry()     -> void;
    auto trap_mcck_entry()    -> void;
    auto trap_restart_entry() -> void;
    auto ap_dat_on()          -> void;

    /// @brief Kernel-wide stack-protector guard
    extern u64 __stack_chk_guard;
}

namespace arch::s390x::cpu::lowcore {

    [[nodiscard]] auto get_current() noexcept -> zx_lowcore* {
        return reinterpret_cast<zx_lowcore*>(
            zxfoundation::memory::hhdm::phys_to_virt(0ULL));
    }

    [[nodiscard]] auto get_current_phys() noexcept -> zx_lowcore* {
        return nullptr;
    }

    /// @brief Retrieve the lowcore structure pointer for a specific CPU, taking prefixing into account.
    /// @param[in] cpu_id  The logical target CPU ID.
    /// @return Pointer to the lowcore of the target CPU, or nullptr if unregistered.
    [[nodiscard]] auto get_for_cpu(const u16 cpu_id) noexcept -> zx_lowcore* {
        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]]
            return nullptr;

        const u16 current_cpu = *reinterpret_cast<const volatile u16*>(
            zxfoundation::memory::hhdm::phys_to_virt(LC_CPU_ID_OFFSET));

        if (cpu_id == current_cpu) {
            return reinterpret_cast<zx_lowcore*>(
                zxfoundation::memory::hhdm::phys_to_virt(0ULL));
        }

        const u64 phys = __percpu_lowcore_phys[cpu_id].load(
            std::memory_order::acquire);
        if (phys == 0) [[unlikely]]
            return nullptr;

        return reinterpret_cast<zx_lowcore*>(
            zxfoundation::memory::hhdm::phys_to_virt(phys));
    }

    auto register_cpu_lowcore(const u16 cpu_id, const u64 phys_lc) noexcept -> void {
        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]]
            return;
        __percpu_lowcore_phys[cpu_id].store(
            phys_lc, std::memory_order::release);
    }

    [[nodiscard]] auto this_cpu_percpu() noexcept -> percpu::percpu_data& {
        return get_current()->percpu;
    }

    /// @brief Get the current domain pointer from lowcore.
    [[nodiscard]] auto current_domain_ptr() noexcept -> void* {
        const auto* lc = get_current();
        return reinterpret_cast<void*>(lc->current_domain);
    }

    /// @brief Get the current strand pointer from percpu.
    [[nodiscard]] auto current_strand_ptr() noexcept -> void* {
        auto& pcp = this_cpu_percpu();
        return reinterpret_cast<void*>(pcp.current_strand);
    }

    [[nodiscard]] auto cpu_percpu(const u16 cpu_id) noexcept -> percpu::percpu_data* {
        zx_lowcore* lc = get_for_cpu(cpu_id);
        return lc ? &lc->percpu : nullptr;
    }

    auto install_handler_psws(zx_lowcore& lc) noexcept -> void {
        lc.program_new_psw  = psw::make_handler_psw(reinterpret_cast<u64>(trap_pgm_entry));
        lc.external_new_psw = psw::make_handler_psw(reinterpret_cast<u64>(trap_ext_entry));
        lc.io_new_psw       = psw::make_handler_psw(reinterpret_cast<u64>(trap_io_entry));
        lc.mcck_new_psw     = psw::make_handler_psw(reinterpret_cast<u64>(trap_mcck_entry));
        lc.svc_new_psw      = psw::make_handler_psw(reinterpret_cast<u64>(trap_svc_entry));
    }

    /// @brief Install a runtime restart-interrupt handler on the current CPU.
    /// @param[in] fn   Callback invoked by trap_restart_entry on SIGP RESTART.
    /// @param[in] data Opaque cookie forwarded as %r2 to the callback.
    auto register_restart_handler(
        void(*fn)(void*) noexcept,
        void* data
    ) noexcept -> void {
        zx_lowcore* lc = get_current();
        lc->restart_fn   = reinterpret_cast<u64>(fn);
        lc->restart_data = reinterpret_cast<u64>(data);
        lc->restart_psw  = psw::make_handler_psw(
            reinterpret_cast<u64>(trap_restart_entry));
    }

    /// @brief Initialize the BSP lowcore.  Must be called once after HHDM is active.
    auto init_bsp(const init::zxfl::protocol& proto) noexcept -> void {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        const u32 bsp_prefix_phys = processor::get_prefix();
#pragma GCC diagnostic pop

        register_cpu_lowcore(0, static_cast<u64>(bsp_prefix_phys));

        zx_lowcore* lc = get_current();

        lc->async_stack   = reinterpret_cast<u64>(s_boot_async_stack   + sizeof(s_boot_async_stack)); 
        lc->mcck_stack    = reinterpret_cast<u64>(s_boot_mcck_stack    + sizeof(s_boot_mcck_stack));
        lc->nodat_stack   = reinterpret_cast<u64>(s_boot_nodat_stack   + sizeof(s_boot_nodat_stack));
        lc->restart_stack = reinterpret_cast<u64>(s_boot_restart_stack + sizeof(s_boot_restart_stack));
        lc->kernel_stack  = proto.kernel_stack_top;
        lc->kernel_asce   = proto.cr1_snapshot;
        lc->user_asce     = 0ULL;
        lc->ap_cr0        = proto.cr0_snapshot;
        lc->ap_cr13       = proto.cr13_snapshot;

        install_handler_psws(*lc);

        lc->restart_psw   = psw::zx_psw{ .mask = psw::PSW_MASK_DISABLED_WAIT,
                                          .addr = psw::PSW_STD_HALT_ADDR };

        lc->percpu.prefix_phys  = static_cast<u64>(bsp_prefix_phys);
        lc->percpu.cpu_id       = 0;
        lc->percpu.cpu_addr     = processor::cpu_addr();
        for (u32 i = 0; i < proto.cpu_count; ++i) {
            if (proto.cpu_map[i].cpu_addr == lc->percpu.cpu_addr) {
                lc->percpu.cpu_type = proto.cpu_map[i].type;
                lc->percpu.drawer_id = proto.cpu_map[i].drawer_id;
                lc->percpu.core_id = proto.cpu_map[i].core_id;
                lc->percpu.smt_id = proto.cpu_map[i].smt_id;
                lc->percpu.topology_evidence = proto.cpu_map[i].topology_evidence;
                lc->percpu.capacity = (proto.cpu_map[i].capacity == 0U) ? 100U : proto.cpu_map[i].capacity;
                break;
            }
        }
        lc->percpu.kernel_stack = proto.kernel_stack_top;
        lc->percpu.lock_depth   = 0;
        lc->percpu.preempt_count = 0;
        lc->percpu.irq_nesting  = 0;
        lc->percpu.nucleus_nesting = 0;

        lc->stack_canary  = __stack_chk_guard;
        lc->cpu_nr        = 0;
        lc->current_domain = 0;
        lc->percpu.current_strand = 0;
        lc->preempt_count = 0;
    }

    /// @brief Prepare an AP lowcore for SIGP RESTART.
    /// @param lc   HHDM-mapped pointer to the AP's lowcore page.
    /// @param p    AP initialization parameters.
    auto init_ap(zx_lowcore& lc, const ap_init_params& p) noexcept -> void {
        lc.restart_psw   = psw::make_ap_restart_psw(p.ap_entry_phys);

        lc.ap_cr0        = p.ap_cr0;
        lc.ap_cr13       = p.kernel_asce;
        lc.kernel_asce   = p.kernel_asce;

        lc.restart_stack = p.ap_stack_phys;

        lc.return_psw    = psw::zx_psw{ .mask = psw::PSW_MASK_KERNEL_DAT,
                                         .addr = reinterpret_cast<u64>(ap_dat_on) };

        install_handler_psws(lc);

        lc.percpu.prefix_phys   = p.phys_lc;
        lc.percpu.cpu_id        = p.cpu_id;
        lc.percpu.cpu_addr      = p.cpu_addr;
        lc.percpu.ap_stack_phys = p.ap_stack_phys;
        lc.percpu.lock_depth    = 0;
        lc.percpu.preempt_count = 0;
        lc.percpu.irq_nesting   = 0;
        lc.percpu.nucleus_nesting = 0;

        lc.percpu.current_strand = 0;
        lc.stack_canary = __stack_chk_guard;
        lc.cpu_nr       = static_cast<u32>(p.cpu_id);
    }

} // namespace arch::s390x::cpu::lowcore
