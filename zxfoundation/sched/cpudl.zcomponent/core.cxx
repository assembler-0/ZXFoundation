/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/cpudl.cxx
/// @brief Per-system per-CPU earliest-deadline index

module zxfoundation.sched.cpudl.core;
import zxfoundation.base.types;
import zxfoundation.base.config;
import arch.s390x.cpu.cpumask;
import lib.rbtree;
import lib.cpumask;
import lib.error;
import lib.container_of;
import lib.stl;

namespace zxfoundation::sched::cpudl::detail {

    using zxfoundation::base::config::CONFIG_ZX_MAX_CPUS;

    /// @brief Per-CPU rb-tree node embedding for the global DEADLINE index.
    /// @details Each CPU has exactly one slot in the global tree.
    struct cpudl_node {
        lib::rb_node            rb_link{};
        u64                     deadline_ns{0ULL};
        u16                     cpu{0xFFFFU};
        bool                    is_set{false};
    };

    /// @brief The global earliest-deadline index root + per-CPU storage.
    struct alignas(64) cpudl_index {
        lib::rb_root            root{};
        std::array<cpudl_node, CONFIG_ZX_MAX_CPUS> nodes{};
        /// @brief Cached snapshot of the earliest-deadline CPU + deadline ns.
        std::atomic<u64>  earliest_ns{0ULL};
        std::atomic<u32> earliest_cpu{0xFFFF'FFFFU};
    };

    no_export cpudl_index g_cpudl{};

    /// @brief rb-tree comparator: nodes keyed by (deadline_ns)
    [[nodiscard]] inline auto less(const cpudl_node& a, const cpudl_node& b) noexcept -> bool {
        if (a.deadline_ns != b.deadline_ns) return a.deadline_ns < b.deadline_ns;
        return a.cpu < b.cpu;
    }

} // namespace detail

namespace zxfoundation::sched::cpudl {

    /// @brief Initialize the per-CPU deadline index.  Boot-time only.
    auto init() noexcept -> void {
        detail::g_cpudl.root = {};
        for (u32 c = 0U; c < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++c) {
            detail::g_cpudl.nodes[c].rb_link = {};
            detail::g_cpudl.nodes[c].deadline_ns = 0ULL;
            detail::g_cpudl.nodes[c].cpu = static_cast<u16>(c);
            detail::g_cpudl.nodes[c].is_set = false;
        }
        detail::g_cpudl.earliest_ns.store(0ULL, std::memory_order::release);
        detail::g_cpudl.earliest_cpu.store(0xFFFF'FFFFU, std::memory_order::release);
    }

    /// @brief Insert / update a CPU's top-deadline entry.
    /// @param[in] cpu          Logical CPU.
    /// @param[in] deadline_ns  Absolute deadline in nanoseconds (TOD-derived).
    /// @note Caller must hold the global @c deadline_lock.
    auto set_dl(u16 cpu, u64 deadline_ns) noexcept -> void {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] return;
        auto& node = detail::g_cpudl.nodes[cpu];
        if (node.is_set) {
            lib::rb_erase(&node.rb_link, detail::g_cpudl.root);
        }
        node.deadline_ns = deadline_ns;
        node.is_set = true;

        const auto cmp = [](const lib::rb_node* n, const u64 key) noexcept -> i32 {
            const auto& x = *lib::container_of<detail::cpudl_node, lib::rb_node, &detail::cpudl_node::rb_link>(n);
            return lib::rb_cmp(x.deadline_ns, key);
        };
        lib::rb_insert(detail::g_cpudl.root, &node.rb_link, deadline_ns, cmp);

        auto* first = lib::rb_first(detail::g_cpudl.root);
        if (first != nullptr) {
            auto* first_node = lib::container_of<detail::cpudl_node, lib::rb_node, &detail::cpudl_node::rb_link>(first);
            detail::g_cpudl.earliest_ns.store(first_node->deadline_ns,
                                                   std::memory_order::release);
            detail::g_cpudl.earliest_cpu.store(first_node->cpu,
                                                     std::memory_order::release);
        }
    }

    /// @brief Clear a CPU's entry from the deadline index.
    /// @param[in] cpu Logical CPU.
    auto clear_dl(u16 cpu) noexcept -> void {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] return;
        auto& node = detail::g_cpudl.nodes[cpu];
        if (!node.is_set) return;
        lib::rb_erase(&node.rb_link, detail::g_cpudl.root);
        node.deadline_ns = 0ULL;
        node.is_set = false;
        auto* first = lib::rb_first(detail::g_cpudl.root);
        if (first != nullptr) {
            auto* first_node = lib::container_of<detail::cpudl_node, lib::rb_node, &detail::cpudl_node::rb_link>(first);
            detail::g_cpudl.earliest_ns.store(first_node->deadline_ns,
                                                   std::memory_order::release);
            detail::g_cpudl.earliest_cpu.store(first_node->cpu,
                                                     std::memory_order::release);
        } else {
            detail::g_cpudl.earliest_ns.store(0ULL, std::memory_order::release);
            detail::g_cpudl.earliest_cpu.store(0xFFFF'FFFFU, std::memory_order::release);
        }
    }

    /// @brief Return the CPU id with the earliest currently-set deadline, or
    ///        @c CONFIG_ZX_MAX_CPUS when none set.
    [[nodiscard]] auto first_cpu() noexcept -> u16 {
        const u32 cpu = detail::g_cpudl.earliest_cpu.load(
            std::memory_order::acquire);
        if (cpu == 0xFFFF'FFFFU) return static_cast<u16>(zxfoundation::base::config::CONFIG_ZX_MAX_CPUS);
        return static_cast<u16>(cpu);
    }

    /// @brief Return the latest @c set deadline among all CPUs
    /// @note O(N) walk — ran once per push, results cached by the caller.
    [[nodiscard]] auto find_latest(const ::lib::cpumask& affinity) noexcept -> u16 {
        u16 chosen = static_cast<u16>(zxfoundation::base::config::CONFIG_ZX_MAX_CPUS);
        u64 latest = 0ULL;
        for (u32 c = 0; c < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++c) {
            const auto& node = detail::g_cpudl.nodes[c];
            if (!node.is_set) continue;
            if (!affinity.test(c, std::memory_order::acquire)) continue;
            if (!arch::s390x::cpu::cpumask::cpu_online(static_cast<u16>(c))) continue;
            if (node.deadline_ns >= latest) {
                latest = node.deadline_ns;
                chosen = static_cast<u16>(c);
            }
        }
        return chosen;
    }

} // namespace zxfoundation::sched::cpudl