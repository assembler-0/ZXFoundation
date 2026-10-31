/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/lowcore.zcomponent/lowcore.cxx
/// @brief Lowcore (prefix area) management: access, initialization, and AP setup.

module arch.s390x.cpu.lowcore;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.types;
import arch.s390x.init.zxfl.protocol;
import zxfoundation.memory.hhdm;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sys.zinit.core;
import zxfoundation.init.storage.core;
import zxfoundation.sync.qspinlock.mcs;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions


/// @brief Physical addresses of each CPU's lowcore page, indexed by logical CPU ID.
static std::array<std::atomic<u64>, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> __percpu_lowcore_phys{};
alignas(16) static std::byte s_boot_async_stack[arch::s390x::cpu::processor::ASYNC_STACK_SIZE]{};
alignas(16) static std::byte s_boot_mcck_stack[arch::s390x::cpu::processor::MCCK_STACK_SIZE]{};
alignas(16) static std::byte s_boot_nodat_stack[arch::s390x::cpu::processor::NODAT_STACK_SIZE]{};
alignas(16) static std::byte s_boot_restart_stack[arch::s390x::cpu::processor::STACK_SIZE]{};

extern "C" {
    auto trap_pgm_entry()     -> void;
    auto trap_ext_entry()     -> void;
    auto trap_io_entry()      -> void;
    auto trap_svc_entry()     -> void;
    auto trap_mcck_entry()    -> void;
    auto trap_restart_entry() -> void;
    auto ap_dat_on()          -> void;

    /// @brief Kernel-owned BSP boot stack (in nucleus .bss), defined in head64.S.
    extern u8 __kernel_boot_stack[];

    /// @brief Kernel-wide stack-protector guard
    extern u64 __stack_chk_guard;
}

namespace arch::s390x::cpu::lowcore {

    /// @brief Return the current CPU's prefixed lowcore mapping.
    /// @return HHDM pointer whose low 8 KiB are redirected by PoP prefixing.
    [[nodiscard]] auto get_current() noexcept -> zlowcore* {
        return reinterpret_cast<zlowcore*>(
            zxfoundation::memory::hhdm::phys_to_virt(0ULL));
    }

    /// @brief Return the lowcore at absolute storage address zero.
    /// @return HHDM pointer whose real address equals the current Prefix
    ///        Register, causing PoP prefixing to select absolute address 0.
    [[nodiscard]] auto get_absolute() noexcept -> zlowcore* {
        return reinterpret_cast<zlowcore*>(
            zxfoundation::memory::hhdm::phys_to_virt(get_current_phys()));
    }

    /// @brief Read the physical base currently installed in the Prefix Register.
    /// @return Prefix base used by the executing CPU, in physical address form.
    [[nodiscard]] auto get_current_phys() noexcept -> u64 {
        return static_cast<u64>(processor::get_prefix());
    }

    /// @brief Retrieve the lowcore structure pointer for a specific CPU, taking prefixing into account.
    /// @param[in] cpu_id  The logical target CPU ID.
    /// @return Pointer to the lowcore of the target CPU, or nullptr if unregistered.
    [[nodiscard]] auto get_for_cpu(const u16 cpu_id) noexcept -> zlowcore* {
        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]]
            return nullptr;

        const u16 current_cpu = *reinterpret_cast<const volatile u16*>(
            zxfoundation::memory::hhdm::phys_to_virt(LC_CPU_ID_OFFSET));

        if (cpu_id == current_cpu) {
            return reinterpret_cast<zlowcore*>(
                zxfoundation::memory::hhdm::phys_to_virt(0ULL));
        }

        const u64 phys = __percpu_lowcore_phys[cpu_id].load(
            std::memory_order::acquire);
        if (phys == 0) [[unlikely]]
            return nullptr;

        return reinterpret_cast<zlowcore*>(
            zxfoundation::memory::hhdm::phys_to_virt(phys));
    }

    auto register_cpu_lowcore(const u16 cpu_id, const u64 phys_lc) noexcept -> void {
        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]]
            return;
        __percpu_lowcore_phys[cpu_id].store(
            phys_lc, std::memory_order::release);
    }

    auto unregister_cpu_lowcore(const u16 cpu_id, const u64 phys_lc) noexcept -> void {
        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]]
            return;

        auto& binding = __percpu_lowcore_phys[cpu_id];
        u64 expected = phys_lc;
        discard_value binding.compare_exchange_strong(
            expected,
            0ULL,
            std::memory_order::acq_rel,
            std::memory_order::acquire);
    }

    /// @brief Get the current domain pointer from lowcore.
    [[nodiscard]] auto current_domain_ptr() noexcept -> void* {
        const auto* lc = get_current();
        return reinterpret_cast<void*>(lc->get<"current_domain">());
    }

    /// @brief Get the current strand pointer from percpu.
    [[nodiscard]] auto current_strand_ptr() noexcept -> void* {
        auto& pcp = percpu::this_cpu();
        return reinterpret_cast<void*>(pcp.current_strand);
    }

    auto install_handler_psws(zlowcore& lc) noexcept -> void {
        lc.set<"program_new_psw">(psw::make_handler_psw(reinterpret_cast<u64>(trap_pgm_entry)));
        lc.set<"external_new_psw">(psw::make_handler_psw(reinterpret_cast<u64>(trap_ext_entry)));
        lc.set<"io_new_psw">(psw::make_handler_psw(reinterpret_cast<u64>(trap_io_entry)));
        lc.set<"mcck_new_psw">(psw::make_handler_psw(reinterpret_cast<u64>(trap_mcck_entry)));
        lc.set<"supervisor_call_new_psw">(psw::make_handler_psw(reinterpret_cast<u64>(trap_svc_entry)));
    }

    /// @brief Install a runtime restart-interrupt handler on the current CPU.
    /// @param[in] fn   Callback invoked by trap_restart_entry on SIGP RESTART.
    /// @param[in] data Opaque cookie forwarded as %r2 to the callback.
    auto register_restart_handler(
        void(*fn)(void*) noexcept,
        void* data
    ) noexcept -> void {
        zlowcore* lc = get_current();
        lc->set<"restart_fn">(fn);
        lc->set<"restart_data">(reinterpret_cast<u64>(data));
        lc->set<"restart_new_psw">(psw::make_handler_psw(
            reinterpret_cast<u64>(trap_restart_entry)));
    }

    /// @brief Initialize the BSP lowcore.  Must be called once after HHDM is active.
    auto init_bsp(const init::zxfl::protocol& proto) noexcept -> void {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        const u32 bsp_prefix_phys = processor::get_prefix();
#pragma GCC diagnostic pop

        register_cpu_lowcore(0, static_cast<u64>(bsp_prefix_phys));

        zlowcore* lc = get_current();

        lc->set<"async_stack">  (reinterpret_cast<u64>(s_boot_async_stack   + sizeof(s_boot_async_stack)));
        lc->set<"mcck_stack">   (reinterpret_cast<u64>(s_boot_mcck_stack    + sizeof(s_boot_mcck_stack)));
        lc->set<"nodat_stack">  (reinterpret_cast<u64>(s_boot_nodat_stack   + sizeof(s_boot_nodat_stack)));
        lc->set<"restart_stack">(reinterpret_cast<u64>(s_boot_restart_stack + sizeof(s_boot_restart_stack)));
        lc->set<"kernel_stack"> (reinterpret_cast<u64>(&__kernel_boot_stack[processor::STACK_SIZE]));
        lc->set<"kernel_asce">  (proto.cr1_snapshot);
        lc->set<"user_asce">    (0ULL);
        lc->set<"ap_cr0">       (proto.cr0_snapshot);
        lc->set<"ap_cr13">      (proto.cr13_snapshot);

        install_handler_psws(*lc);

        lc->set<"restart_new_psw">(psw::zpsw{ "mask"_f = psw::PSW_MASK_DISABLED_WAIT,
                                           "address"_f = psw::PSW_STD_HALT_ADDR });

        lc->set<"cpu_id">(0);
        lc->set<"percpu_ptr">(reinterpret_cast<u64>(&percpu::at(0)));

        percpu::percpu_data& pcp = percpu::at(0);
        pcp.cpu_id       = 0;
        pcp.cpu_addr     = processor::cpu_addr();
        for (u32 i = 0; i < proto.cpu_count; ++i) {
            if (proto.cpu_map[i].cpu_addr == pcp.cpu_addr) {
                pcp.cpu_type = proto.cpu_map[i].type;
                pcp.drawer_id = proto.cpu_map[i].drawer_id;
                pcp.core_id = proto.cpu_map[i].core_id;
                pcp.smt_id = proto.cpu_map[i].smt_id;
                pcp.topology_evidence = proto.cpu_map[i].topology_evidence;
                pcp.capacity = (proto.cpu_map[i].capacity == 0U) ? 100U : proto.cpu_map[i].capacity;
                break;
            }
        }
        pcp.kernel_stack = reinterpret_cast<u64>(&__kernel_boot_stack[processor::STACK_SIZE]);
        pcp.lock_depth   = 0;
        pcp.preempt_count = 0;
        pcp.irq_nesting  = 0;
        pcp.nucleus_nesting = 0;
        pcp.current_strand = 0;

        lc->set<"stack_canary">(__stack_chk_guard);
        lc->set<"current_domain">(0);
    }

    auto zi_lowcore_init_bsp(const zxfoundation::sys::zinit::zi_desc&) noexcept
        -> zxfoundation::sys::zinit::zi_result
    {
        init_bsp(zxfoundation::init::storage::get_protocol());
        return {};
    }
    __zinit__(early, p70, zi_lowcore_init_bsp, fatal)

    /// @brief Prepare an AP lowcore for SIGP RESTART.
    /// @param lc   HHDM-mapped pointer to the AP's lowcore page.
    /// @param p    AP initialization parameters.
    auto init_ap(zlowcore& lc, const ap_init_params& p) noexcept -> void {
        lc.set<"restart_new_psw">(psw::make_ap_restart_psw(p.ap_entry_phys));
        lc.set<"return_psw">(psw::make_handler_psw(reinterpret_cast<u64>(ap_dat_on)));
        lc.set<"ap_cr0">(p.ap_cr0);
        lc.set<"ap_cr13">(p.kernel_asce);
        lc.set<"kernel_asce">(p.kernel_asce);
        lc.set<"restart_stack">(p.ap_stack_phys);

        install_handler_psws(lc);

        lc.set<"cpu_id">(p.cpu_id);
        lc.set<"percpu_ptr">(reinterpret_cast<u64>(&percpu::at(p.cpu_id)));

        percpu::percpu_data& pcp = percpu::at(p.cpu_id);
        pcp.cpu_id        = p.cpu_id;
        pcp.cpu_addr      = p.cpu_addr;
        pcp.ap_stack_phys = p.ap_stack_phys;
        pcp.lock_depth    = 0;
        pcp.preempt_count = 0;
        pcp.irq_nesting   = 0;
        pcp.nucleus_nesting = 0;
        pcp.current_strand = 0;

        lc.set<"stack_canary">(__stack_chk_guard);
    }

} // namespace arch::s390x::cpu::lowcore
