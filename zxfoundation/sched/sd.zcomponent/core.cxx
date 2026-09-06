/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/domain_tree.cxx
/// @brief Hierarchical balancing-domain builder and traversal helpers.

module zxfoundation.sched.sd.core;
import zxfoundation.sched.sd.types;
import zxfoundation.sched.topology.types;
import zxfoundation.sched.topology.core;
import zxfoundation.sched.policy;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sched.policy;
import lib.cpumask;
import lib.error;
import lib.stl;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.lowcore;

namespace zxfoundation::sched::sd::detail {

    /// @brief Copy a cpumask from one to another where operator= is deleted
    ///        because the backing storage is atomic.  Uses reset + for_each + set.
    inline auto cpumask_copy(::lib::cpumask& dst, const ::lib::cpumask& src) noexcept -> void {
        dst.reset();
        src.for_each([&](u16 c) noexcept -> void {
            dst.set(c, std::memory_order::relaxed);
        });
    }

    using namespace zxfoundation::sched::sd;
    using zxfoundation::sched::topology::cpu_topology_record;

    /// @brief Static descriptor pool for @c sched_domain descriptors.
    alignas(64) sched_domain g_domain_pool[zxfoundation::base::config::CONFIG_ZX_MAX_CPUS][SCHED_DOMAIN_MAX_LEVEL]{};

    /// @brief Static descriptor pool for @c sched_group nodes.
    alignas(64) std::array<sched_group, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS * 4U> g_group_pool{};

    inline auto reset_domain(sched_domain& sd) noexcept -> void {
        sd.level               = sd_level::smt;
        sd.flags               = sd_flag::load_balance;
        sd.span_cpus.reset();
        sd.groups              = nullptr;
        sd.parent              = nullptr;
        sd.child               = nullptr;
        sd.balance_interval_ns = 1'000'000ULL;
        sd.last_balance_tick   = 0ULL;
        sd.nr_balance_failed   = 0U;
        sd.cache_hot_time_ns   = 500'000ULL;
        sd.imbalance_pct       = 125U;
        sd.nr_busy_cpus        = 0U;
        sd.domain_load         = 0ULL;
        sd.domain_capacity     = 0U;
        sd.fresh_built         = true;
    }

    inline auto reset_group(sched_group& sg) noexcept -> void {
        sg.cpus.reset();
        sg.next             = nullptr;
        sg.group_capacity   = 0U;
        sg.avg_load         = 0ULL;
        sg.nr_balance_failed = 0U;
        sg.has_running      = true;
    }

    inline auto group_index(u16 cpu, u32 group_n) noexcept -> u32 {
        return static_cast<u32>(cpu) * 4U + (group_n & 3U);
    }

    /// @brief Build the @c sched_group ring for one level/CPU.
    inline auto build_group_ring(sched_domain& sd, u16 local_cpu, u32 level_idx) noexcept -> void {
        sched_group* ring_head = nullptr;
        sched_group* ring_tail = nullptr;
        u32 group_n = 0U;

        sd.span_cpus.for_each([&](u16 cpu) noexcept -> void {
            auto& sg = g_group_pool[group_index(local_cpu, group_n)];
            reset_group(sg);
            sg.cpus.set(cpu, std::memory_order::relaxed);
            sg.group_capacity = 100U;  // canonical nonsense capacity until refresh_cpu_record updates.
            if (ring_tail == nullptr) {
                ring_head = &sg;
                ring_tail = &sg;
            } else {
                ring_tail->next = &sg;
                ring_tail = &sg;
            }
            ++group_n;
        }, std::memory_order::relaxed);
        if (ring_tail != nullptr) {
            ring_tail->next = ring_head; // circular
        }
        sd.groups = ring_head;
        discard_value level_idx;
    }

} // namespace detail

namespace zxfoundation::sched::sd {

    /// @brief Build one CPU's @c sched_domain tree.
    /// @param[in] cpu          Target CPU.
    /// @param[in] topo_record  Pointer to that CPU's @c cpu_topology_record.
    /// @param[in] node_mask    NUMA node mask for the CPU's local node.
    /// @param[out] out_tree    Output per-CPU tree (zeroed and populated).
    auto tree_build(
        u16 cpu,
        const zxfoundation::sched::topology::cpu_topology_record* topo_record,
        const ::lib::cpumask& node_mask,
        sched_domain_tree& out_tree
    ) noexcept -> void {
        for (auto & level : out_tree.levels) {
            level = nullptr;
        }
        out_tree.max_built_level = 0U;

        if (!topo_record) return;

        // The level build order is SMT → MC → DIE → BOOK → NUMA.
        const bool online = arch::s390x::cpu::cpumask::cpu_online(cpu);
        if (!online) return;

        sd_level levels[] = {sd_level::smt, sd_level::mc, sd_level::die, sd_level::book, sd_level::numa};

        u64 last_built = 0ULL;
        sched_domain* prev = nullptr;
        for (u32 lvl = 0; lvl < SCHED_DOMAIN_MAX_LEVEL; ++lvl) {
            auto& sd = detail::g_domain_pool[cpu][lvl];
            detail::reset_domain(sd);
            sd.level = levels[lvl];
            sd.last_balance_tick = 0ULL;

            // Build the candidate span per level.
            switch (levels[lvl]) {
                case sd_level::smt: {
                    if (topo_record->smt_evidence ==
                        zxfoundation::sched::topology_evidence::degraded) {
                        sd.span_cpus.set(cpu, std::memory_order::relaxed);
                        sd.flags = sd_flag::load_balance | sd_flag::share_pkg_resources;
                    } else {
                        detail::cpumask_copy(sd.span_cpus, node_mask);  // siblings are subset of node
                        sd.flags = sd_flag::load_balance | sd_flag::balance_newidle
                                 | sd_flag::share_cpu_capacity | sd_flag::share_pkg_resources
                                 | sd_flag::prefer_sibling;
                    }
                    break;
                }
                case sd_level::mc: {
                    // MC: same NUMA node CPUs within socket/drawer.
                    detail::cpumask_copy(sd.span_cpus, node_mask);
                    sd.flags = sd_flag::load_balance | sd_flag::balance_exec
                             | sd_flag::balance_fork | sd_flag::balance_newidle;
                    break;
                }
                case sd_level::die: {
                    // DIE: drawer-wide (same as node on s390x with book topology).
                    detail::cpumask_copy(sd.span_cpus, node_mask);
                    sd.flags = sd_flag::load_balance | sd_flag::balance_exec
                             | sd_flag::balance_fork;
                    break;
                }
                case sd_level::book: {
                    detail::cpumask_copy(sd.span_cpus, node_mask);
                    sd.flags = sd_flag::load_balance;
                    break;
                }
                case sd_level::numa: {
                    detail::cpumask_copy(sd.span_cpus, node_mask);
                    sd.flags = sd_flag::load_balance | sd_flag::balance_exec
                             | sd_flag::balance_fork | sd_flag::balance_wake
                             | sd_flag::numa;
                    break;
                }
            }
            // Build sibling groups for this domain's candidate set.
            detail::build_group_ring(sd, cpu, lvl);

            // Cascade parent/child pointers.
            sd.child = prev;
            if (prev != nullptr) {
                prev->parent = &sd;
            }
            prev = &sd;
            out_tree.levels[lvl] = &sd;
            last_built = static_cast<u64>(lvl);
        }
        out_tree.max_built_level = static_cast<u32>(last_built);
    }

    /// @brief Walk up the hierarchy from @c base returning the first domain
    ///        that has all of the requested flag bits set.
    /// @param[in] base  Bottom-most domain in the search.
    /// @param[in] flag  Flag bitmask required in the result.
    /// @return Pointer to the highest ancestor that satisfies the flag test
    [[nodiscard]] auto highest_flag_domain(const sched_domain* base, sd_flag flag) noexcept -> const sched_domain* {
        const sched_domain* result = nullptr;
        for (const sched_domain* sd = base; sd != nullptr; sd = sd->parent) {
            if ((static_cast<u32>(sd->flags) & static_cast<u32>(flag)) == static_cast<u32>(flag)) {
                result = sd;
            }
        }
        return result;
    }

    /// @brief Walk down the hierarchy from @c base returning the lowest domain
    ///        which has all of @c flag set.
    /// @param[in] base  Top-most domain in the search.
    /// @param[in] flag  Flag bitmask required in the result.
    [[nodiscard]] auto lowest_flag_domain(const sched_domain* base, sd_flag flag) noexcept -> const sched_domain* {
        for (const sched_domain* sd = base; sd != nullptr; sd = sd->child) {
            if ((static_cast<u32>(sd->flags) & static_cast<u32>(flag)) == static_cast<u32>(flag)) {
                return sd;
            }
        }
        return nullptr;
    }

    /// @brief Test whether @c cpu is in @c sd.span_cpus.
    [[nodiscard]] auto domain_contains_cpu(const sched_domain& sd, u16 cpu) noexcept -> bool {
        return sd.span_cpus.test(cpu, std::memory_order::relaxed);
    }

    /// @brief Cache-hot predicate: keeping a strand on its current CPU for
    ///        @c cache_hot_time_ns since last run.
    [[nodiscard]] auto is_cache_hot(const sched_domain& sd, u64 elapsed_ns) noexcept -> bool {
        return elapsed_ns < sd.cache_hot_time_ns;
    }

} // namespace zxfoundation::sched::sd