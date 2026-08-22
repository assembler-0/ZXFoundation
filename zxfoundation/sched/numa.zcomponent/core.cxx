/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/node_distance.cxx
/// @brief STSI 15.1.x topology-list probe — feeds PMM's single-source SLIT.

module zxfoundation.sched.numa.core;
import zxfoundation.sched.topology.types;
import zxfoundation.sched.topology.core;
import zxfoundation.memory.numa.types;
import zxfoundation.memory.numa;
import zxfoundation.base.types;
import zxfoundation.base.config;
import arch.s390x.cpu.stsi;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.lowcore;
import lib.cpumask;
import lib.error;
import std;

namespace zxfoundation::sched::numa::detail {

    using namespace zxfoundation::memory::numa;

    /// @brief Per-CPU NUMA node assignment cached for fast topology lookups.
    no_export std::array<u8, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_stsi_cpu_to_node{};

    /// @brief Build a synthetic 16×16 SLIT using @c cpu_relation placement tiers.
    auto build_synthetic_slit_to(slit_matrix& out) noexcept -> void {
        for (u32 a = 0; a < MAX_NUMA_NODES; ++a) {
            for (u32 b = 0; b < MAX_NUMA_NODES; ++b) {
                if (a == b) out.distances[a][b] = 10U;
                else if (a == NUMA_NODE_BOOT || b == NUMA_NODE_BOOT) out.distances[a][b] = 20U;
                else out.distances[a][b] = 40U;
            }
        }
        const auto max_cpus = zxfoundation::base::config::CONFIG_ZX_MAX_CPUS;
        for (u32 i = 0; i < max_cpus; ++i) {
            const auto* rec = zxfoundation::sched::topology::record_for(static_cast<u16>(i));
            if (!rec) {
                g_stsi_cpu_to_node[i] = NUMA_NODE_BOOT;
                continue;
            }
            g_stsi_cpu_to_node[i] = (rec->numa_node < MAX_NUMA_NODES) ? rec->numa_node : NUMA_NODE_BOOT;
        }
    }

    /// @brief Which nesting level's container ID we treat as the NUMA node.
    inline constexpr u8 NUMA_NODE_TOPOLOGY_LEVEL = 1;

    /// @brief Parse one STSI 15.1.x output block into the staging SLIT matrix.
    auto parse_topology_configuration_block(
        std::span<const std::byte, PAGE_SIZE> page,
        u32 threads_per_core,
        slit_matrix& out_matrix,
        bool& wrote_node
    ) noexcept -> arch::s390x::cpu::stsi::topology_parse_result {
        using namespace arch::s390x::cpu::stsi;

        const configuration_topology_view view{page};
        const auto result = view.entries().visit_cpu_entries(
            [&](const topology_cpu_record& rec) {
                u8 node_id = rec.path.id_at(NUMA_NODE_TOPOLOGY_LEVEL).value_or(NUMA_NODE_BOOT);
                if (node_id >= MAX_NUMA_NODES) node_id = NUMA_NODE_BOOT;

                rec.cpu.for_each_cpu(threads_per_core, [&](u16 cpu_id) {
                    if (cpu_id < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[likely]] {
                        g_stsi_cpu_to_node[cpu_id] = node_id;
                        wrote_node = true;
                    }
                });
            });

        for (u32 a = 0; a < MAX_NUMA_NODES; ++a) {
            for (u32 b = 0; b < MAX_NUMA_NODES; ++b) {
                if (a == b)                 out_matrix.distances[a][b] = 10U;
                else if (a == 0 || b == 0)  out_matrix.distances[a][b] = 20U;
                else                        out_matrix.distances[a][b] = 40U;
            }
        }
        return result;
    }

    /// @brief Probe STSI 15.1.x
    /// @return True on success (CC=0); false on CC=3 (not available).
    auto probe_topology_configuration(
        u32 sel2,
        u32 threads_per_core,
        slit_matrix& out_matrix,
        bool& wrote_node
    ) noexcept -> bool {
        using namespace arch::s390x::cpu;
        alignas(PAGE_SIZE) std::array<std::byte, PAGE_SIZE> buffer{};
        auto* info = reinterpret_cast<stsi::configuration_topology*>(buffer.data());
        const auto r = stsi::invoke<
            stsi::configuration_topology,
            stsi::stsi_fc::current_configuration_level,
            stsi::selector1::configuration_level_fc
        >(*info, sel2);
        if (!r) return false;
        if (*r == 3) return false;   // CC=3 → not available

        const auto parsed =
            parse_topology_configuration_block(buffer, threads_per_core, out_matrix, wrote_node);
        return parsed.ok();
    }

    auto detected_threads_per_core() noexcept -> u32 {
        using namespace arch::s390x::cpu::stsi;

        alignas(PAGE_SIZE) lpar_cpus lpar_page;
        if (const auto r = invoke<
                lpar_cpus, stsi_fc::logical_partition_information,
                selector1::configuration_level_multi_cpu, selector2_2::configuration_level_all_cpu
            >(lpar_page); r && *r == 0) {
            if (lpar_page.mt_installed) {
                return static_cast<u32>(lpar_page.pmstid) + 1U;
            }
        }

        alignas(PAGE_SIZE) std::array<std::byte, PAGE_SIZE> basic_page{};
        if (const auto r = invoke<
                std::array<std::byte, PAGE_SIZE>, stsi_fc::basic_machine_information,
                selector1::configuration_level_multi_cpu, selector2_2::configuration_level_all_cpu
            >(basic_page); r && *r == 0) {
            const basic_machine_cpus_view view{basic_page};
            if (view.header().mt_installed) {
                return static_cast<u32>(view.header().max_tid_core) + 1U;
            }
        }

        return 1U;
    }

} // namespace detail

namespace zxfoundation::sched::numa {

    /// @brief Probe STSI 15.1.x and install the resulting SLIT into PMM.
    auto slit_load() noexcept -> void {
        for (u8& i : detail::g_stsi_cpu_to_node) {
            i = zxfoundation::memory::numa::NUMA_NODE_BOOT;
        }
        detail::slit_matrix matrix{};
        bool stsi_ok = false;
        bool wrote_node = false;
        for (u32 sel2 = 1; sel2 <= 3; ++sel2) {
            if (detail::probe_topology_configuration(sel2, detail::detected_threads_per_core(), matrix, wrote_node)) {
                stsi_ok = true;
            }
        }
        if (!stsi_ok) {
            detail::build_synthetic_slit_to(matrix);
        }
        zxfoundation::memory::numa::slit_install(
            matrix,
            stsi_ok && wrote_node);
    }

} // namespace zxfoundation::sched::numa