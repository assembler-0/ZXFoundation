/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/percpu.cxxm
/// @brief Per-CPU data initialization.

module arch.s390x.cpu.percpu;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.lowcore;
import zxfoundation.memory.hhdm;
import zxfoundation.memory.numa.types;
import zxfoundation.memory.pmm.types;
import zxfoundation.base.config;
import zxfoundation.base.types;
import lib.stl;

namespace arch::s390x::cpu::percpu {

    /// @brief Offset of zlowcore::percpu_ptr within the 8 KiB prefix area.
    no_export constexpr u64 LC_PERCPU_PTR_OFFSET = 0x0400UL;

    /// @brief Fixed-size, cpu_id-indexed storage for every CPU's percpu_data.
    alignas(processor::CACHE_LINE_SIZE) no_export
        std::array<percpu_data, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_percpu_data{};

    [[nodiscard]] auto at(const u16 cpu_id) noexcept -> percpu_data& {
        if (cpu_id >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]]
            psw::disabled_wait();
        return g_percpu_data[cpu_id];
    }

    /// @brief Reference to the current CPU's per-CPU block, reached through
    ///        zlowcore::percpu_ptr (one pointer indirection via prefixing).
    [[nodiscard]] auto this_cpu() noexcept -> percpu_data& {
        const u64 ptr = *reinterpret_cast<const volatile u64*>(
            zxfoundation::memory::hhdm::phys_to_virt(LC_PERCPU_PTR_OFFSET));
        return *reinterpret_cast<percpu_data*>(ptr);
    }

    /// @brief get current cpu's ZXFL_CPU_TYPE_* value
    [[nodiscard]] auto cpu_type() noexcept -> u8 {
        return this_cpu().cpu_type;
    }

    /// @brief get current cpu id
    [[nodiscard]] auto cpu_id() noexcept -> u16 {
        return this_cpu().cpu_id;
    }

    [[nodiscard]] auto cpu_percpu(const u16 cpu_id) noexcept -> percpu::percpu_data* {
        if (lowcore::get_for_cpu(cpu_id) == nullptr) [[unlikely]]
            return nullptr;
        return &at(cpu_id);
    }

    /// @brief Finish BSP per-CPU initialization.
    auto init_bsp() noexcept -> void {
        percpu_data& pcp = this_cpu();

        pcp.self_phys = zxfoundation::memory::hhdm::virt_to_phys(
            reinterpret_cast<u64>(&pcp));

        pcp.rcu_gp_seq         = 0;
        pcp.rcu_qs_seq         = 0;
        pcp.in_rcu_read_side   = 0;

        pcp.ipi_pending_count  = 0;

        for (u32 n = 0; n < zxfoundation::memory::numa::MAX_NUMA_NODES; ++n)
            for (u32 z = 0; z < static_cast<u32>(zxfoundation::memory::pmm::zone::count); ++z)
                for (u32 o = 0; o <= zxfoundation::memory::pmm::PCP_MAX_ORDER; ++o) {
                    pcp.pcp.lists[n][z][o].head_phys = 0;
                    pcp.pcp.lists[n][z][o].count     = 0;
                    pcp.pcp.lists[n][z][o]._pad      = 0;
                }
        pcp.drawer_id = 0;
        pcp.core_id = 0;
        pcp.smt_id = 0;
        pcp.topology_evidence = 0;
        pcp.capacity = 100;
    }

    /// @brief Finish AP per-CPU initialization.
    /// @param pcp  Reference to the AP's percpu_data slot (percpu::at(cpu_id)).
    auto init_ap(percpu_data& pcp) noexcept -> void {
        pcp.self_phys = zxfoundation::memory::hhdm::virt_to_phys(
            reinterpret_cast<u64>(&pcp));

        pcp.rcu_gp_seq        = 0;
        pcp.rcu_qs_seq        = 0;
        pcp.in_rcu_read_side  = 0;

        pcp.ipi_pending_count = 0;

        pcp.current_domain    = 0;
        pcp.user_asce         = 0;

        for (u32 n = 0; n < zxfoundation::memory::numa::MAX_NUMA_NODES; ++n)
            for (u32 z = 0; z < static_cast<u32>(zxfoundation::memory::pmm::zone::count); ++z)
                for (u32 o = 0; o <= zxfoundation::memory::pmm::PCP_MAX_ORDER; ++o) {
                    pcp.pcp.lists[n][z][o].head_phys = 0;
                    pcp.pcp.lists[n][z][o].count     = 0;
                    pcp.pcp.lists[n][z][o]._pad      = 0;
                }
        pcp.core_id = pcp.cpu_id & 0xFFU;
        pcp.smt_id = 0;
        pcp.topology_evidence = 0;
        pcp.capacity = 100;
        // drawer_id and topology details are set by smp::init_aps() from the boot protocol after this call.
    }

    /// @brief Return the lowercase short prefix for the current CPU type.
    /// Used by printk to format e.g. [cp00], [ifl02], [ziip03].
    [[nodiscard]] auto cpu_type_prefix() noexcept -> std::string_view {
        switch (this_cpu().cpu_type) {
            case 1:  return "cp";     // ZXFL_CPU_TYPE_CP
            case 2:  return "il";     // ZXFL_CPU_TYPE_IFL
            case 3:  return "if";     // ZXFL_CPU_TYPE_ICF
            case 4:  return "ip";     // ZXFL_CPU_TYPE_ZIIP
            case 5:  return "ap";     // ZXFL_CPU_TYPE_ZAAP
            default: return "t?";     // fallback
        }
    }

} // namespace arch::s390x::cpu::percpu
