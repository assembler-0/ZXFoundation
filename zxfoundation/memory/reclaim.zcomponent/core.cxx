/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/memory/reclaim.zcomponent/core.cxx
/// @brief Unified reclaim: walks page_cache + all VMAs via the SCOMS

module zxfoundation.memory.reclaim.core;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.numa;
import zxfoundation.memory.numa.types;
import zxfoundation.memory.oom;
import zxfoundation.memory.page_cache;
import zxfoundation.memory.vm;
import zxfoundation.memory.vm.types;
import arch.s390x.mmu.tlb;
import zxfoundation.dgp.domain.core;
import zxfoundation.base.types;
import zxfoundation.dgp.domain.types;
import lib.error;
import std;


namespace zxfoundation::memory::reclaim {

    namespace detail { reclaim_fn g_reclaimer{nullptr}; }
    using namespace detail;

    /// @brief Install the reclaim callback used by kswapd and allocation retry.
    /// @param[in] callback Callback that isolates and reclaims pages.
    /// @return Success or invalid argument.
    [[nodiscard]] auto install(reclaim_fn callback) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (!callback) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        g_reclaimer = callback;
        return {};
    }

    /// @brief Reclaim pages on one NUMA node.
    /// @param[in] node Preferred NUMA node.
    /// @param[in] target_pages Minimum requested progress.
    /// @return Pages reclaimed or a provider-not-ready error.
    [[nodiscard]] auto reclaim_node(u8 node, u32 target_pages) noexcept
        -> std::expected<u32, lib::kernel_error> {
        if (!numa::node_id_is_valid(node) || target_pages == 0U) return std::unexpected(
            lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        u32 reclaimed = page_cache::reclaim_unmapped(target_pages);
        if (reclaimed >= target_pages || !g_reclaimer) return reclaimed;

        const u32 remaining = target_pages - reclaimed;
        const u32 provider_reclaimed = g_reclaimer(node, remaining);
        if (provider_reclaimed > remaining) return target_pages;
        return reclaimed + provider_reclaimed;
    }

    /// @brief Reclaim enough pages to satisfy an allocation retry.
    /// @param[in] node Preferred NUMA node.
    /// @param[in] target_pages Minimum progress.
    /// @return Success when progress meets the target.
    [[nodiscard]] auto reclaim_for_allocation(u8 node, u32 target_pages) noexcept
        -> std::expected<void, lib::kernel_error> {
        auto result = reclaim_node(node, target_pages);
        if (!result) return std::unexpected(result.error());
        if (*result < target_pages) return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::zone_exhausted));
        return {};
    }

    /// @brief Evict every resident page whose physical address lies in
    ///        [phys_start, phys_end).
    /// @return Number of frames actually freed back to PMM.
    [[nodiscard]] auto reclaim_range(u64 phys_start, u64 phys_end) noexcept -> u32 {
        u32 freed = 0U;

        freed += page_cache::evict_range(phys_start, phys_end);

        dgp::domain::domain_for_each(
            [&](dgp::domain::domain& d) noexcept -> bool {
                freed += vm::vm_space_drain_phys_range(
                    d.vm_space, phys_start, phys_end);
                return true;
            });

        if (freed == 0U) {
            discard_value oom::kill_highest(256U);
        }
        return freed;
    }

} // namespace zxfoundation::memory::reclaim

