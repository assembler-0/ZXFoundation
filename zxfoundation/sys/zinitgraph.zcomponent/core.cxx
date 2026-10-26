/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/zinitgraph.zcomponent/core.cxx
/// @brief zinitgraph core — DAG construction, topological execution, profiling.

module zxfoundation.sys.zinitgraph.core;
import zxfoundation.base.types;
import zxfoundation.sys.zinit.core;
import zxfoundation.sys.printk.core;
import arch.s390x.cpu.clock;
import zxfoundation.base.cmdline.core;
import lib.error;
import lib.container_of;
import lib.list;
import lib.stl;

namespace zxfoundation::sys::zinitgraph::detail {

    /// @brief Cached toggle; default enabled until the cmdline says otherwise.
    no_export bool g_enabled = true;

    /// @brief Maximum number of graph nodes, including internal barrier nodes.
    no_export constexpr usize kMaxNodes = 640U;
    /// @brief Maximum number of graph edges.
    no_export constexpr usize kMaxEdges = 2048U;

    no_export zg_node  g_node_pool[kMaxNodes];
    no_export u32      g_node_pool_used = 0U;

    no_export zg_edge  g_edge_pool[kMaxEdges];
    no_export u32      g_edge_pool_used = 0U;

    /// @brief Barrier node per class level.
    no_export zg_node* g_barriers[static_cast<usize>(zinit::zi_class::max)] = {};

    /// @brief Stable names for barrier nodes, ordered as zi_class.
    no_export const char* const kBarrierNames[static_cast<usize>(zinit::zi_class::max)] = {
        "__barrier_early",
        "__barrier_memory",
        "__barrier_core",
        "__barrier_dgp",
        "__barrier_sched",
        "__barrier_smp",
        "__barrier_io",
        "__barrier_drivers",
        "__barrier_fs",
        "__barrier_late",
    };

    /// @brief Reset both pools.  Callers must discard any previous graph.
    auto reset_pools() noexcept -> void {
        g_node_pool_used = 0U;
        g_edge_pool_used = 0U;
        for (auto& b : g_barriers) b = nullptr;
    }

    /// @brief Allocate a zero-initialized node from the no_export pool.
    /// @return Node pointer, or nullptr when the pool is exhausted.
    /// @note Pool elements carry in-class initializers, so their list heads
    ///       and state fields are valid at first use without placement new.
    [[nodiscard]] auto node_alloc() noexcept -> zg_node* {
        if (g_node_pool_used >= kMaxNodes) return nullptr;
        return &g_node_pool[g_node_pool_used++];
    }

    /// @brief Allocate a zero-initialized edge from the no_export pool.
    [[nodiscard]] auto edge_alloc() noexcept -> zg_edge* {
        if (g_edge_pool_used >= kMaxEdges) return nullptr;
        return &g_edge_pool[g_edge_pool_used++];
    }

    /// @brief Find a node by exact name.
    /// @param[in] g     Graph instance.
    /// @param[in] name  Name to locate.
    /// @return Matching node, or nullptr when absent.
    [[nodiscard]] auto find_node(zg_graph& g, std::string_view name) noexcept -> zg_node* {
        for (auto& n : g.nodes.iter<zg_node, &zg_node::all>()) {
            if (n.name == name) return &n;
        }
        return nullptr;
    }

    /// @brief Link a directed edge and account for the destination in-degree.
    /// @param[in,out] g    Graph instance.
    /// @param[in]     src  Source node; runs first.
    /// @param[in]     dst  Destination node; runs second.
    /// @return Error when the edge pool is exhausted.
    [[nodiscard]] auto add_edge(zg_graph& g, zg_node* src, zg_node* dst) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (!src || !dst || src == dst)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        zg_edge* e = edge_alloc();
        if (!e)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::out_of_range));
        e->src = src;
        e->dst = dst;
        src->out_edges.push_back(e->src_link);
        dst->in_edges.push_back(e->dst_link);
        dst->unsatisfied += 1U;
        g.edge_count += 1U;
        return {};
    }

    /// @brief Ready-queue ordering predicate.
    /// @note Sorts by (class, barrier-flag, prio) so that ready nodes execute
    ///       in the same relative order the flat engine would choose; the
    ///       barrier of class K always sorts behind real nodes of class K.
    [[nodiscard]] auto queue_less(const zg_node& lhs, const zg_node& rhs) noexcept -> bool {
        if (lhs.cls != rhs.cls)
            return static_cast<u8>(lhs.cls) < static_cast<u8>(rhs.cls);
        if (lhs.is_barrier != rhs.is_barrier)
            return lhs.is_barrier < rhs.is_barrier;
        return static_cast<u8>(lhs.prio) < static_cast<u8>(rhs.prio);
    }

    /// @brief Insert a node into the ready queue, keeping it sorted.
    auto ready_push(zg_graph& g, zg_node& n) noexcept -> void {
        for (auto it = g.ready.begin(); it != g.ready.end(); ++it) {
            lib::list_node* marker = *it;
            if (zg_node& cur = *lib::container_of<zg_node, lib::list_node, &zg_node::queue>(marker);
                queue_less(n, cur)) {
                g.ready.insert_before(*marker, n.queue);
                return;
                }
        }
        g.ready.push_back(n.queue);
    }

    /// @brief Pop the highest-priority ready node.
    [[nodiscard]] auto ready_pop(zg_graph& g) noexcept -> zg_node* {
        lib::list_node* nl = g.ready.pop_front();
        if (!nl) return nullptr;
        return lib::container_of<zg_node, lib::list_node, &zg_node::queue>(nl);
    }

}

namespace zxfoundation::sys::zinitgraph {

    [[nodiscard]] auto enabled() noexcept -> bool {
        return detail::g_enabled;
    }

    auto register_cmdline() noexcept -> void {
        zxfoundation::base::cmdline::add({
            .name    = "zx.sys.init.initgraph",
            .type    = zxfoundation::base::cmdline::value_type::callback,
            .handler = [](std::string_view value) noexcept -> bool {
                detail::g_enabled = !(value == "0" || value == "off" || value == "false");
                return true;
            },
            .help    = "Enable the init graph overlay (1, default) or run flat (0)",
        });
    }

    auto build(zg_graph& g) noexcept -> std::expected<void, lib::kernel_error> {
        detail::reset_pools();

        const auto table = zinit::descriptors();
        if (table.empty())
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));

        // Materialize descriptor nodes; reject duplicate names outright because
        // two same-named nodes would make dep resolution ambiguous.
        for (const auto& d : table) {
            if (detail::find_node(g, d.name))
                return std::unexpected(
                    lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

            zg_node* n = detail::node_alloc();
            if (!n)
                return std::unexpected(
                    lib::kernel_error::from_generic(lib::generic_error::out_of_range));
            n->desc = &d;
            n->name = d.name;
            n->cls  = d.cls;
            n->prio = d.prio;
            g.nodes.push_back(n->all);
            g.node_count += 1U;
        }

        // Materialize per-class barrier nodes.
        for (usize k = 0; k < static_cast<usize>(zinit::zi_class::max); ++k) {
            zg_node* b = detail::node_alloc();
            if (!b)
                return std::unexpected(
                    lib::kernel_error::from_generic(lib::generic_error::out_of_range));
            b->desc       = nullptr;
            b->name       = detail::kBarrierNames[k];
            b->cls        = static_cast<zinit::zi_class>(k);
            b->prio       = zinit::zi_prio::lowest;
            b->is_barrier = 1U;
            detail::g_barriers[k] = b;
            g.nodes.push_back(b->all);
            g.node_count += 1U;
        }

        // Class barrier K-1 -> barrier K; keeps the chain intact for empty classes.
        for (usize k = 1; k < static_cast<usize>(zinit::zi_class::max); ++k) {
            if (auto r = detail::add_edge(g, detail::g_barriers[k - 1], detail::g_barriers[k]); !r)
                return r;
        }

        // Wire descriptor nodes: class barrier edges, deps, entails.
        for (auto& n : g.nodes.iter<zg_node, &zg_node::all>()) {
            if (n.is_barrier) continue;

            const auto k = static_cast<usize>(n.cls);
            // This node starts only after the previous class barrier has passed.
            if (k > 0)
                if (auto r = detail::add_edge(g, detail::g_barriers[k - 1], &n); !r)
                    return r;
            // The class barrier passes only after every node of this class is done.
            if (auto r = detail::add_edge(g, &n, detail::g_barriers[k]); !r)
                return r;

            if (n.desc->deps) {
                for (usize i = 0; n.desc->deps[i] != nullptr; ++i) {
                    zg_node* dep = detail::find_node(g, n.desc->deps[i]);
                    if (!dep) {
                        printk::pnotice(
                            "zinitgraph: dep '{}' of '{}' unresolved; edge skipped",
                            std::string_view{n.desc->deps[i]}, n.name);
                        continue;
                    }
                    if (auto r = detail::add_edge(g, dep, &n); !r)
                        return r;
                }
            }

            if (n.desc->entails) {
                for (usize i = 0; n.desc->entails[i] != nullptr; ++i) {
                    zg_node* succ = detail::find_node(g, n.desc->entails[i]);
                    if (!succ) {
                        printk::pnotice(
                            "zinitgraph: entail '{}' of '{}' unresolved; edge skipped",
                            std::string_view{n.desc->entails[i]}, n.name);
                        continue;
                    }
                    if (auto r = detail::add_edge(g, &n, succ); !r)
                        return r;
                }
            }
        }

        return {};
    }

    [[nodiscard]] auto run(zg_graph& g) noexcept -> u32 {
        u32 failed = 0U;

        // Seed the queue with every prerequisite-free node.
        for (auto& n : g.nodes.iter<zg_node, &zg_node::all>()) {
            if (n.unsatisfied == 0U && n.state == node_state::pending) {
                n.state = node_state::ready;
                detail::ready_push(g, n);
            }
        }

        while (zg_node* cur = detail::ready_pop(g)) {
            cur->state = node_state::running;

            if (cur->is_barrier) {
                cur->state = node_state::done;
            } else {
                cur->start_tod = arch::s390x::cpu::clock::stckf();

                auto r = zinit::execute(*cur->desc);

                cur->end_tod  = arch::s390x::cpu::clock::stckf();
                cur->duration = cur->end_tod - cur->start_tod;

                if (!r) {
                    cur->err   = r.error();
                    cur->state = node_state::failed;
                    g.failed += 1U;
                    failed   += 1U;
                } else {
                    cur->state = node_state::done;
                }
            }

            for (auto& e : cur->out_edges.iter<zg_edge, &zg_edge::src_link>()) {
                zg_node* succ = e.dst;
                if (succ->unsatisfied > 0U) succ->unsatisfied -= 1U;
                if (succ->unsatisfied == 0U && succ->state == node_state::pending) {
                    succ->state = node_state::ready;
                    detail::ready_push(g, *succ);
                }
            }
        }

        // Residue in pending/ready means a cycle or a dangling edge blocked them.
        u32 unreached = 0U;
        for (auto& n : g.nodes.iter<zg_node, &zg_node::all>()) {
            if (n.state == node_state::pending || n.state == node_state::ready) {
                n.state = node_state::unreached;
                unreached += 1U;
                if (n.desc)
                    printk::perr("zinitgraph: node '{}' unreached", n.name);
            }
        }
        g.unreached = unreached;

        return failed;
    }

    auto print_profile(const zg_graph& g) noexcept -> void {
        printk::pinfo("zinitgraph: profile ({} node(s), {} edge(s))",
                      g.node_count, g.edge_count);
        for (auto& n : g.nodes.iter<zg_node, &zg_node::all>()) {
            if (n.is_barrier) continue;
            std::string_view state_str = "pending";
            switch (n.state) {
                case node_state::pending:   state_str = "pending";   break;
                case node_state::ready:     state_str = "ready";     break;
                case node_state::running:   state_str = "running";   break;
                case node_state::done:      state_str = "done";      break;
                case node_state::failed:    state_str = "failed";    break;
                case node_state::unreached: state_str = "unreached"; break;
            }
            const u64 us = n.duration >> 12;
            printk::pinfo("zinitgraph:   {:<44} {:>10} tod  {:>8} us  {}",
                          n.name, n.duration, us, state_str);
        }
    }

    auto print_mermaid(const zg_graph& g) noexcept -> void {
        printk::pinfo("zinitgraph: mermaid");
        printk::pinfo("flowchart TD");
        for (auto& n : g.nodes.iter<zg_node, &zg_node::all>()) {
            if (n.is_barrier) continue;
            printk::pinfo("  {}", n.name);
        }
        for (auto& n : g.nodes.iter<zg_node, &zg_node::all>()) {
            for (auto& e : n.out_edges.iter<zg_edge, &zg_edge::src_link>()) {
                if (!e.dst || e.dst->is_barrier) continue;
                printk::pinfo("  {} --> {}", n.name, e.dst->name);
            }
        }
        printk::pinfo("zinitgraph: end mermaid");
    }

} // namespace zxfoundation::sys::zinitgraph
