/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/memory/hhdm.zcomponent/core.cxx
/// @brief HHDM (Higher-Half Direct Map) translation helpers — implementation.

module zxfoundation.memory.hhdm.core;
import zxfoundation.memory.hhdm.types;
import zxfoundation.base.types;
import zxfoundation.sys.zinit.core;
import zxfoundation.init.storage.core;
import arch.s390x.init.zxfl.protocol;
import lib.error;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace zxfoundation::memory::hhdm::detail {

    no_export u64 g_phys_coverage_end{0U};

} // namespace zxfoundation::memory::hhdm::detail

namespace zxfoundation::memory::hhdm {

    /// @brief Record the physical end covered by the active loader-built HHDM.
    /// @param[in] phys_end Exclusive physical end address translated by the current HHDM DAT.
    /// @note The value is supplied by ZXFL after it constructs the initial DAT tables.
    /// @warning This function records coverage only; it does not alter DAT tables or flush translations.
    auto set_phys_coverage_end(u64 phys_end) noexcept -> void {
        detail::g_phys_coverage_end = phys_end;
    }

    /// @brief Return the exclusive physical end covered by the active HHDM.
    /// @return Exclusive physical coverage end, or zero if no contract has been recorded.
    [[nodiscard]] auto phys_coverage_end() noexcept -> u64 {
        return detail::g_phys_coverage_end;
    }

    /// @brief Test whether a physical range is covered by the active HHDM.
    /// @param[in] phys Physical start address.
    /// @param[in] bytes Range size in bytes.
    /// @return True when the full physical interval has a direct-map translation.
    /// @warning A zero recorded coverage end is treated as not covered so callers fail closed.
    [[nodiscard]] auto phys_range_is_mapped(u64 phys, u64 bytes) noexcept -> bool {
        const u64 coverage_end = detail::g_phys_coverage_end;
        if (coverage_end == 0U || bytes == 0U) return false;
        if (phys >= coverage_end) return false;
        const u64 range_end = phys + bytes;
        if (range_end < phys) return false;
        return range_end <= coverage_end;
    }

    /// @brief Translate a physical address (raw integer) to its HHDM virtual counterpart.
    /// @param[in] phys  Raw physical address.
    /// @return  HHDM virtual address: @p phys + KERNEL_VIRTUAL_OFFSET.
    [[nodiscard]] auto phys_to_virt(u64 phys) noexcept -> u64 {
        return phys + KERNEL_VIRTUAL_OFFSET;
    }

    /// @brief Translate an HHDM virtual address back to its physical counterpart.
    /// @param[in] virt  HHDM virtual address (must be >= KERNEL_VIRTUAL_OFFSET).
    /// @return  Physical address: @p virt - KERNEL_VIRTUAL_OFFSET.
    [[nodiscard]] auto virt_to_phys(u64 virt) noexcept -> u64 {
        return virt - KERNEL_VIRTUAL_OFFSET;
    }

    /// @brief Initialize the HHDM coverage contract exported by ZXFL.
    auto zi_hhdm_contract(const sys::zinit::zi_desc&) noexcept
        -> sys::zinit::zi_result
    {
        const auto& boot = zxfoundation::init::storage::get_protocol();
        if (boot.hhdm_phys_coverage_end == 0U
            || (boot.hhdm_phys_coverage_end & (4096ULL - 1ULL)) != 0U
            || boot.hhdm_phys_coverage_end < boot.pgtbl_pool_end
            || boot.hhdm_phys_coverage_end < boot.kernel_phys_end) [[unlikely]] {
            return std::unexpected(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol));
            }

        const auto* map = reinterpret_cast<const arch::s390x::init::zxfl::mem_region*>(
            boot.mem_map_addr);
        if (boot.mem_map_count > arch::s390x::init::zxfl::ZXFL_MEM_MAP_MAX
            || (boot.mem_map_count != 0U && !map)) [[unlikely]] {
            return std::unexpected(
               lib::kernel_error::from_init(lib::init_error::code::invalid_protocol));
            }
        u64 highest_ram_end = 0U;
        for (u32 i = 0U; i < boot.mem_map_count; ++i) {
            if (map[i].length != 0U
                && map[i].base > ~0ULL - map[i].length) [[unlikely]] {
                return std::unexpected(
                    lib::kernel_error::from_init(lib::init_error::code::invalid_protocol));
                }
            if (const u64 region_end = map[i].base + map[i].length;
                region_end > highest_ram_end) highest_ram_end = region_end;
        }
        if (highest_ram_end > boot.hhdm_phys_coverage_end) [[unlikely]] {
            return std::unexpected(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol));
        }

        set_phys_coverage_end(boot.hhdm_phys_coverage_end);
        return {};
    }
    __zinit__(mm, p00, zi_hhdm_contract, fatal)

} // namespace zxfoundation::memory::hhdm

