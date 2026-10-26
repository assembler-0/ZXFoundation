/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/cpupri.zcomponent/core.cxx
/// @brief Per-system CPU priority index used by the RT scheduler.

module zxfoundation.sched.cpupri.core;
import zxfoundation.base.types;
import zxfoundation.base.config;
import lib.cpumask;
import lib.error;
import lib.stl;
import arch.s390x.cpu.cpumask;

namespace zxfoundation::sched::cpupri::detail {

    /// @brief Sentinel priority used for "no RT work" buckets.
    inline constexpr u32 CPUPRI_INVALID = 0xFFU;

    /// @brief The CPU priority index state — one mask per priority bucket.
    struct alignas(64) cpupri_index {
        /// @brief Mask of CPUs currently running that priority.
        std::array<::lib::cpumask, CPUPRI_NR_PRIORITIES> pri_mask;
        /// @brief Mask of CPUs with no RT work currently runnable.
        ::lib::cpumask            free_mask;
    };

    cpupri_index g_cpupri{};

    /// @brief Per-CPU cached "lowest currently-running RT priority".
    std::array<std::atomic<u8>, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_cpu_top_pri{};

} // namespace detail

namespace zxfoundation::sched::cpupri {

    /// @brief Initialize the cpupri index.
    auto init() noexcept -> void {
        for (u32 p = 0; p < CPUPRI_NR_PRIORITIES; ++p) {
            detail::g_cpupri.pri_mask[p].reset();
        }
        detail::g_cpupri.free_mask.reset();
        for (u32 c = 0U; c < zxfoundation::base::config::CONFIG_ZX_MAX_CPUS; ++c) {
            detail::g_cpu_top_pri[c].store(detail::CPUPRI_INVALID,
                                              std::memory_order::relaxed);
            if (arch::s390x::cpu::cpumask::cpu_online(static_cast<u16>(c))) {
                detail::g_cpupri.free_mask.set(c, std::memory_order::relaxed);
            }
        }
    }

    /// @brief Update the index for an RT enqueue/dequeue on a CPU.
    /// @param[in] cpu      CPU whose top RT priority changed.
    /// @param[in] old_pri  Previous top priority (0..127 or CPUPRI_INVALID).
    /// @param[in] new_pri  New top priority (0..127 or CPUPRI_INVALID).
    auto pri_set(u16 cpu, u8 old_pri, u8 new_pri) noexcept -> void {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) [[unlikely]] return;

        if (old_pri != detail::CPUPRI_INVALID && old_pri < CPUPRI_NR_PRIORITIES) {
            detail::g_cpupri.pri_mask[old_pri].clear(cpu, std::memory_order::release);
        } else {
            detail::g_cpupri.free_mask.clear(cpu, std::memory_order::release);
        }

        if (new_pri != detail::CPUPRI_INVALID && new_pri < CPUPRI_NR_PRIORITIES) {
            detail::g_cpupri.pri_mask[new_pri].set(cpu, std::memory_order::release);
        } else {
            detail::g_cpupri.free_mask.set(cpu, std::memory_order::release);
        }

        detail::g_cpu_top_pri[cpu].store(new_pri, std::memory_order::release);
    }

    /// @brief Find the CPU with the lowest RT priority at or below a threshold,
    ///        restricted by an affinity mask.
    /// @param[in] pri_threshold RT priority ceiling — CPUs running strictly
    ///            higher priority than @c pri_threshold are not returned.
    /// @param[in] affinity       Allowed CPU mask.
    /// @return CPU id (an arbitrary lowest-priority candidate), or
    ///         @c CONFIG_ZX_MAX_CPUS when no CPU qualifies.
    [[nodiscard]] auto find_fitness(u8 pri_threshold, const ::lib::cpumask& affinity) noexcept -> u16 {
        for (u32 p = 0; p <= pri_threshold; ++p) {
            const auto& mask = detail::g_cpupri.pri_mask[p];
            auto first = mask.first_set();
            while (first.has_value()) {
                const u16 cpu = static_cast<u16>(*first);
                if (affinity.test(cpu, std::memory_order::acquire)
                    && arch::s390x::cpu::cpumask::cpu_online(cpu)) {
                    return cpu;
                }
                first = mask.next_set(*first);
            }
        }
        // Fall back to free CPUs (no RT work) that match the affinity.
        auto free_first = detail::g_cpupri.free_mask.first_set();
        while (free_first.has_value()) {
            const u16 cpu = static_cast<u16>(*free_first);
            if (affinity.test(cpu, std::memory_order::acquire)
                && arch::s390x::cpu::cpumask::cpu_online(cpu)) {
                return cpu;
            }
            free_first = detail::g_cpupri.free_mask.next_set(*free_first);
        }
        return static_cast<u16>(zxfoundation::base::config::CONFIG_ZX_MAX_CPUS);
    }

    /// @brief Read the cached top RT priority of a CPU.
    [[nodiscard]] auto pri_top(u16 cpu) noexcept -> u8 {
        if (cpu >= zxfoundation::base::config::CONFIG_ZX_MAX_CPUS) return detail::CPUPRI_INVALID;
        return detail::g_cpu_top_pri[cpu].load(std::memory_order::acquire);
    }

} // namespace zxfoundation::sched::cpupri