/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/percpu.cxxm
/// @brief Per-CPU data initialization.

module arch.s390x.cpu.percpu;
import arch.s390x.cpu.processor;
import zxfoundation.memory.hhdm;
import zxfoundation.memory.numa.types;
import zxfoundation.memory.pmm.types;
import zxfoundation.base.config;
import zxfoundation.base.types;

namespace arch::s390x::cpu::percpu {

    /// @brief Offset of the per-CPU data block within the 8 KiB prefix area.
    no_export constexpr u64 LC_PERCPU_OFFSET = 0x0400UL;

    /// @brief Reference to the current CPU's per-CPU block
    [[nodiscard]] auto this_cpu() noexcept -> percpu_data& {
        return *reinterpret_cast<percpu_data*>(
            zxfoundation::memory::hhdm::phys_to_virt(0ULL) + LC_PERCPU_OFFSET);
    }

    /// @brief get current cpu's ZXFL_CPU_TYPE_* value
    [[nodiscard]] auto cpu_type() noexcept -> u8 {
        return this_cpu().cpu_type;
    }

    /// @brief get current cpu id
    [[nodiscard]] auto cpu_id() noexcept -> u16 {
        return this_cpu().cpu_id;
    }

    /// @brief Finish BSP per-CPU initialization.
    auto init_bsp() noexcept -> void {
        percpu_data& pcp = this_cpu();

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

        pcp.softirq_pending = 0;
    }

    /// @brief Finish AP per-CPU initialization.
    /// @param pcp  HHDM-mapped pointer to the AP's pcp.
    auto init_ap(percpu_data& pcp) noexcept -> void {
        pcp.rcu_gp_seq        = 0;
        pcp.rcu_qs_seq        = 0;
        pcp.in_rcu_read_side  = 0;

        pcp.ipi_pending_count = 0;

        pcp.softirq_pending   = 0;
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
