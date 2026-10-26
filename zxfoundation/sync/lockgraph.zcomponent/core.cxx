/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sync/lockgraph.zcomponent/core.cxx
/// @brief Lockgraph runtime validator

module zxfoundation.sync.lockgraph.core;
import zxfoundation.sync.lockgraph.types;
import zxfoundation.sync.lockrank;
import arch.s390x.cpu.percpu;
import zxfoundation.sys.syschk.core;
import zxfoundation.base.cmdline.core;
import lib.error;
import lib.stl;

namespace zxfoundation::sync::lockgraph::detail {

#if !defined(ZX_NO_LOCKGRAPH)

    constexpr u32 RANK_COUNT = static_cast<u32>(lock_rank::count_);

    struct inst_edge {
        const void* from{nullptr};
        const void* to{nullptr};
        lock_rank   rank{};
        u8          _pad[3]{};
    };

    // Rank dependency edges: bit j of g_rank_edges[i] means "rank i may be
    // followed by rank j". Ranks are the node taxonomy (§20.4).
    u32 g_rank_edges[RANK_COUNT]{};

    // Same-rank instance ordering edges (the acquire_equal contract).
    inst_edge g_inst_edges[MAX_INSTANCE_EDGES]{};
    u32       g_inst_edge_count{0};

    lockgraph_counters g_counters{};
    bool g_enabled{true};
    u32  g_report_seq{0};

    /// @brief Bounded BFS reachability over the rank edge bitmap.
    [[nodiscard]] auto rank_reaches(u32 from, u32 to) noexcept -> bool {
        bool visited[RANK_COUNT]{};
        u32  queue[RANK_COUNT];
        u32  qh = 0, qt = 0;
        visited[from] = true;
        queue[qt++] = from;
        while (qh < qt) {
            const u32 r = queue[qh++];
            if (r == to) return true;
            const u32 bits = g_rank_edges[r];
            for (u32 i = 0; i < RANK_COUNT; ++i) {
                if ((bits & (1U << i)) != 0U && !visited[i]) {
                    visited[i] = true;
                    queue[qt++] = i;
                }
            }
        }
        return false;
    }

    /// @brief Rate-limited violation report (§20.4: system_check warning
    ///        + held-chain dump; first 8 always, then every 32nd).
    auto report(const char* what, const lockdep_map& map) noexcept -> void {
        ++g_report_seq;
        const bool emit = (g_counters.reports < 8U) || ((g_report_seq % 32U) == 0U);
        if (!emit) {
            ++g_counters.suppressed;
            return;
        }
        ++g_counters.reports;
        zxfoundation::sys::syschk::syschk_warning(lib::kernel_error::from_locking(
            lib::locking_error::deadlock_detected),
            "lockgraph: {} (class {})", what, map.name);
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        for (u32 i = 0; i < pcp.lockgraph_depth; ++i) {
            const auto* m = static_cast<const lockdep_map*>(pcp.lockgraph_held_map[i]);
            if (m) {
                zxfoundation::sys::syschk::syschk_notice(lib::kernel_error::from_locking(
                    lib::locking_error::deadlock_detected),
                    "lockgraph:   held[{}] {} rank {}", i, m->name,
                    static_cast<u32>(m->rank));
            }
        }
    }

#endif // !ZX_NO_LOCKGRAPH

} // namespace zxfoundation::sync::lockgraph::detail

namespace zxfoundation::sync::lockgraph {

#if !defined(ZX_NO_LOCKGRAPH)

    auto init() noexcept -> void {
        detail::g_enabled = true;
    }

    /// @brief Register the boot parameter plane entry (OBL-026).
    /// @note Called from init's cmdline_init before cmdline::parse; the
    ///       zinit-time init() runs after parsing on some graphs.
    auto register_cmdline() noexcept -> void {
        discard_value zxfoundation::base::cmdline::add({
            .name    = "zx.debug.lockgraph",
            .type    = zxfoundation::base::cmdline::value_type::boolean,
            .storage = &detail::g_enabled,
            .help    = "0 disables the runtime lock-order validator",
        });
    }

    auto set_enabled(const bool on) noexcept -> void {
        detail::g_enabled = on;
    }

    [[nodiscard]] auto enabled() noexcept -> bool {
        return detail::g_enabled;
    }

    auto acquire(lockdep_map& map, const void* inst, const bool held_irq_off) noexcept -> void {
        if (!detail::g_enabled) return;
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();

        if (pcp.lockgraph_recursion != 0U) return;
        pcp.lockgraph_recursion = 1U;

        const u8 ctx = (pcp.irq_nesting > 0U) ? ctx_hardirq
                     : (held_irq_off ? ctx_irqoff : ctx_process);
        const u8 before = map.ctx_classes;
        map.ctx_classes = static_cast<u8>(before | ctx);

        constexpr u8 IRQ_CLASSES = static_cast<u8>(ctx_irqoff | ctx_hardirq);
        if ((before & ctx_process) != 0U && (ctx & IRQ_CLASSES) != 0U) {
            ++detail::g_counters.irq_inversions;
            detail::report("irq inversion (irq-context take of an irq-enabled-held class)", map);
        } else if ((before & IRQ_CLASSES) != 0U && ctx == ctx_process) {
            ++detail::g_counters.irq_inversions;
            detail::report("irq inversion (irq-enabled hold of an irq-context class)", map);
        }

        for (u32 i = 0; i < pcp.lockgraph_depth; ++i) {
            const auto* hmap = static_cast<const lockdep_map*>(pcp.lockgraph_held_map[i]);
            const void* hinst = pcp.lockgraph_held_inst[i];
            if (!hmap) continue;
            const u32 hr = static_cast<u32>(hmap->rank);
            const u32 nr = static_cast<u32>(map.rank);
            if (hr == nr) {
                if (hinst == inst) continue;
                bool reverse = false;
                for (u32 e = 0; e < detail::g_inst_edge_count; ++e) {
                    const auto& edge = detail::g_inst_edges[e];
                    if (edge.rank == map.rank && edge.from == inst && edge.to == hinst) {
                        reverse = true;
                        break;
                    }
                }
                if (reverse) {
                    ++detail::g_counters.instance_cycles;
                    detail::report("same-rank instance order cycle", map);
                    continue;
                }
                if (detail::g_inst_edge_count < MAX_INSTANCE_EDGES) {
                    detail::g_inst_edges[detail::g_inst_edge_count++] =
                        detail::inst_edge{hinst, inst, map.rank, {}};
                }
            } else {
                if (detail::rank_reaches(nr, hr)) {
                    ++detail::g_counters.rank_cycles;
                    detail::report("rank dependency cycle", map);
                    continue;
                }
                detail::g_rank_edges[hr] |= (1U << nr);
            }
        }

        ++detail::g_counters.depth_histogram[
            (pcp.lockgraph_depth < 8U) ? pcp.lockgraph_depth : 8U];
        if (pcp.lockgraph_depth < MAX_HELD_CHAIN) {
            pcp.lockgraph_held_inst[pcp.lockgraph_depth] = inst;
            pcp.lockgraph_held_map[pcp.lockgraph_depth] = &map;
            ++pcp.lockgraph_depth;
        } else {
            ++detail::g_counters.chain_overflows;
        }
        pcp.lockgraph_recursion = 0U;
    }

    auto release(const void* inst) noexcept -> void {
        if (!detail::g_enabled) return;
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        if (pcp.lockgraph_recursion != 0U) return;
        for (u32 i = pcp.lockgraph_depth; i > 0U; --i) {
            if (pcp.lockgraph_held_inst[i - 1U] == inst) {
                for (u32 j = i - 1U; j + 1U < pcp.lockgraph_depth; ++j) {
                    pcp.lockgraph_held_inst[j] = pcp.lockgraph_held_inst[j + 1U];
                    pcp.lockgraph_held_map[j] = pcp.lockgraph_held_map[j + 1U];
                }
                --pcp.lockgraph_depth;
                pcp.lockgraph_recursion = 0U;
                return;
            }
        }
        pcp.lockgraph_recursion = 0U;
    }

    [[nodiscard]] auto counters() noexcept -> const lockgraph_counters& {
        return detail::g_counters;
    }

#else // ZX_NO_LOCKGRAPH

    auto init() noexcept -> void {}
    auto set_enabled(bool) noexcept -> void {}
    [[nodiscard]] auto enabled() noexcept -> bool { return false; }
    auto acquire(lockdep_map&, const void*, bool) noexcept -> void {}
    auto release(const void*) noexcept -> void {}
    [[nodiscard]] auto counters() noexcept -> const lockgraph_counters& {
        static const lockgraph_counters zero{};
        return zero;
    }

#endif // ZX_NO_LOCKGRAPH

} // namespace zxfoundation::sync::lockgraph
