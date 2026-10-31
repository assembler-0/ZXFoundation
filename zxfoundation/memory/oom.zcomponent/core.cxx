/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/memory/oom.zcomponent/core.cxx
/// @brief Deterministic global OOM selection and kill callback.

module zxfoundation.memory.oom.core;
import zxfoundation.base.types;
import zxfoundation.dgp.policy.types;
import zxfoundation.sched.context;
import lib.error;
import lib.stl;

namespace zxfoundation::memory::oom::detail {

    static score_fn g_score{nullptr};
    static kill_fn g_kill{nullptr};

}

namespace zxfoundation::memory::oom {

    using namespace detail;

    /// @brief Install OOM scoring and victim termination callbacks.
    /// @param[in] scorer Returns the current domain badness score.
    /// @param[in] killer Marks and signals the selected domain.
    /// @return Success or invalid argument.
    [[nodiscard]] auto install(score_fn scorer, kill_fn killer) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (!scorer || !killer) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        g_score = scorer;
        g_kill = killer;
        return {};
    }

    /// @brief Select and kill the highest-scoring domain in a bounded ID set.
    /// @param[in] domain_count Number of domain IDs to inspect.
    /// @return Victim ID or a checked error.
    [[nodiscard]] auto kill_highest(u32 domain_count) noexcept
        -> std::expected<u32, lib::kernel_error> {
        if (!g_score || !g_kill || domain_count == 0U) return std::unexpected(
            lib::kernel_error::from_generic(lib::generic_error::not_supported));
        u32 victim = 0;
        u64 highest = 0;
        for (u32 id = 0; id < domain_count; ++id) {
            const u64 score = g_score(id);
            if (score > highest) {
                highest = score;
                victim = id;
            }
        }
        if (highest == 0U) return std::unexpected(
            lib::kernel_error::from_memory(lib::memory_error::zone_exhausted));
        if (zxfoundation::sched::context::nesting_depth() == 0U &&
            zxfoundation::sched::context::nucleus_nesting_depth() == 0U &&
            !zxfoundation::sched::context::preempt_disabled()) {
            const auto consult = zxfoundation::dgp::policy::decide(
                0U, zxfoundation::dgp::policy::server_kind::jobs,
                victim, highest,
                zxfoundation::dgp::policy::DECIDE_TIMEOUT_NS);
            if (consult.outcome ==
                zxfoundation::dgp::policy::decide_outcome::deny)
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::zone_exhausted));
        }
        if (!g_kill(victim)) return std::unexpected(
            lib::kernel_error::from_memory(lib::memory_error::zone_exhausted));
        return victim;
    }

} // namespace zxfoundation::memory::oom

