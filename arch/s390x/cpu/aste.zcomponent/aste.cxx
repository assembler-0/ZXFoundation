/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/aste.zcomponent/aste.cxx
/// @brief ASTE/ALE table manager

module arch.s390x.cpu.aste;
import arch.s390x.cpu.ar;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.ctlreg;
import zxfoundation.base.types;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.hhdm;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import arch.s390x.cpu.irq;
import zxfoundation.sys.zinit.core;
import lib.stl;
import :common;

#define __zxmacro_want_zinit
#include macro_extensions

using zxfoundation::memory::pmm::alloc;
using zxfoundation::memory::pmm::zone;
using zxfoundation::memory::pmm::purpose;
using zxfoundation::memory::pmm::page_init;
using zxfoundation::memory::hhdm::phys_to_virt;

namespace pmm = zxfoundation::memory::pmm;

namespace arch::s390x::cpu::aste {

    /// @brief Physical address of the ASTE table (MAX_ASTE_ENTRIES × 64 bytes).
    no_export u64 g_aste_table_phys{0};
    u64 g_aft_phys{0};

    /// @brief Lock protecting all ASTE/ALE table mutations.
    alignas(4) no_export zxfoundation::sync::qspinlock::qspinlock<zxfoundation::sync::lock_rank::aste_table> g_aste_lock{};

    /// @brief Per-slot occupancy for the ASTE table (true = occupied).
    no_export std::array<bool, MAX_ASTE_ENTRIES> g_aste_used{};

    /// @brief Whether the subsystem has been initialized.
    no_export bool g_initialized{false};

    /// @brief Return a typed pointer into the ASTE table by index.
    [[nodiscard]] auto aste_at(u32 idx) noexcept -> aste* {
        auto base = phys_to_virt(g_aste_table_phys);
        return reinterpret_cast<aste*>(base) + idx;
    }

    /// @brief Return a typed pointer into one domain's dispatchable-unit ALE table.
    [[nodiscard]] auto ale_at(const art_context& context, u32 idx) noexcept -> ale* {
        auto base = phys_to_virt(context.ale_table_phys);
        return reinterpret_cast<ale*>(base) + idx;
    }

    /// @brief Linear scan for a free ASTE slot.
    [[nodiscard]] auto find_free_aste() noexcept -> u32 {
        for (u32 i = 0; i < MAX_ASTE_ENTRIES; ++i) {
            if (!g_aste_used[i])
                return i;
        }
        return MAX_ASTE_ENTRIES; // sentinel: no slot found
    }

    /// @brief Linear scan for a free non-reserved ALE slot.
    [[nodiscard]] auto find_free_ale(const art_context& context) noexcept -> u32 {
        for (u32 i = ALET_RESERVED_MAX + 1; i < MAX_ALE_ENTRIES; ++i) {
            if (ale_at(context, i)->is_invalid())
                return i;
        }
        return MAX_ALE_ENTRIES; // sentinel: no slot found
    }

    /// @brief Pages required to hold @p bytes (rounded up).
    [[nodiscard]] constexpr auto pages_for(u64 bytes) noexcept -> u32 {
        return static_cast<u32>((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    }

    /// @brief Buddy order for a given page count (ceil log2).
    [[nodiscard]] constexpr auto order_for_pages(u32 n) noexcept -> u32 {
        u32 order = 0;
        while ((1U << order) < n)
            ++order;
        return order;
    }

    [[nodiscard]] constexpr auto ale_sequence_next(u8 current) noexcept -> u8 {
        const u8 next = static_cast<u8>(current + 1U);
        return next == 0U ? 1U : next;
    }

} // namespace arch::s390x::cpu::aste

/// @name struct ale
/// @{
namespace arch::s390x::cpu::aste {

    //// @brief Test the I-bit (invalid) in word 0.
    [[nodiscard]] auto ale::is_invalid() const noexcept -> bool {
        return (words[0] & (1ULL << 63)) != 0;
    }

    /// @brief Set or clear the I-bit (invalid) in word 0.
    auto ale::set_invalid(bool inv) noexcept -> void {
        constexpr u64 i_mask = 1ULL << 63;
        if (inv)
            words[0] |= i_mask;
        else
            words[0] &= ~i_mask;
    }

    /// @brief Return the real address of the referenced ASTE.
    [[nodiscard]] auto ale::aste_origin() const noexcept -> u64 {
        return ((words[1] >> 38) & 0x01FF'FFFFULL) << 6;
    }

    /// @brief Set the ASTE origin (must be 64-byte aligned).
    auto ale::set_aste_origin(u64 addr) noexcept -> void {
        constexpr u64 aste_origin_mask = 0x01FF'FFFFULL << 38;
        words[1] = (words[1] & ~aste_origin_mask)
                 | ((addr >> 6) & 0x01FF'FFFFULL) << 38;
    }

    [[nodiscard]] auto ale::sequence() const noexcept -> u8 {
        return static_cast<u8>((words[0] >> 48) & 0xFFU);
    }

    auto ale::set_sequence(u8 val) noexcept -> void {
        constexpr u64 sequence_mask = 0xFFULL << 48;
        words[0] = (words[0] & ~sequence_mask)
                  | (static_cast<u64>(val) << 48);
    }

    auto ale::set_fetch_only(bool fetch_only) noexcept -> void {
        constexpr u64 fetch_only_mask = 1ULL << 57;
        if (fetch_only)
            words[0] |= fetch_only_mask;
        else
            words[0] &= ~fetch_only_mask;
    }

    auto ale::set_aste_sequence(u32 val) noexcept -> void {
        words[1] = (words[1] & ~0xFFFF'FFFFULL)
                  | static_cast<u64>(val);
    }

    /// @brief Zero all words.
    auto ale::clear() noexcept -> void {
        words[0] = 0;
        words[1] = 0;
    }

}
/// @}

namespace arch::s390x::cpu::aste {

    /// @brief Initialize the ASTE/ALE subsystem.
    auto zi_aste_init(const zxfoundation::sys::zinit::zi_desc&) noexcept -> zxfoundation::sys::zinit::zi_result {

        constexpr u64 aste_table_size = MAX_ASTE_ENTRIES * sizeof(aste);
        constexpr u32 aste_pages = pages_for(aste_table_size);
        constexpr u32 aste_order = order_for_pages(aste_pages);

        auto aste_result = alloc(aste_order, zone::dma31,
                                 purpose::page_table, page_init::zeroed);
        if (!aste_result) {
            return std::unexpected(aste_result.error());
        }
        g_aste_table_phys = aste_result.value().phys_base;
        auto aft = alloc(0U, zone::dma31, purpose::page_table, page_init::zeroed);
        if (!aft) return ::std::unexpected(aft.error());
        g_aft_phys = aft.value().phys_base;
        *reinterpret_cast<u32*>(phys_to_virt(g_aft_phys)) =
            static_cast<u32>(g_aste_table_phys & 0x7FFFFFC0ULL);

        for (u32 i = 0; i < MAX_ASTE_ENTRIES; ++i) {
            aste_at(i)->set_invalid(true);
            g_aste_used[i] = false;
        }

        g_initialized = true;
        return {};
    }
    __zinit__(core, p60, zi_aste_init, fatal)

    [[nodiscard]] auto art_context_init(art_context& context) noexcept
        -> ::std::expected<void, ::lib::kernel_error> {
        if (!g_initialized) {
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::uninitialized));
        }
        if (context.duct_phys != 0U || context.ale_table_phys != 0U) {
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::invalid_arg));
        }

        auto duct = alloc(0U, zone::dma31, purpose::page_table, page_init::zeroed);
        if (!duct) return ::std::unexpected(duct.error());

        auto ales = alloc(0U, zone::dma31, purpose::page_table, page_init::zeroed);
        if (!ales) {
            pmm::folio_active duct_folio{
                duct->phys_base, 0U, purpose::page_table, zone::dma31};
            pmm::free(static_cast<pmm::folio_active&&>(duct_folio));
            return ::std::unexpected(ales.error());
        }

        context.duct_phys = duct->phys_base;
        context.ale_table_phys = ales->phys_base;
        auto* duct_words = reinterpret_cast<u32*>(phys_to_virt(context.duct_phys));
        auto* entries = reinterpret_cast<ale*>(phys_to_virt(context.ale_table_phys));
        duct_words[0] = static_cast<u32>(g_aste_table_phys & 0x7FFF'FFC0ULL);
        duct_words[4] = static_cast<u32>(context.ale_table_phys & 0x7FFF'FF80ULL) | 1U;
        for (u32 i = 0; i < MAX_ALE_ENTRIES; ++i) {
            entries[i].clear();
            entries[i].set_invalid(true);
            context.ale_sequence[i] = 1U;
        }
        return {};
    }

    auto art_context_destroy(art_context& context) noexcept -> void {
        if (context.ale_table_phys != 0U) {
            pmm::folio_active ales{
                context.ale_table_phys, 0U, purpose::page_table, zone::dma31};
            pmm::free(static_cast<pmm::folio_active&&>(ales));
        }
        if (context.duct_phys != 0U) {
            pmm::folio_active duct{
                context.duct_phys, 0U, purpose::page_table, zone::dma31};
            pmm::free(static_cast<pmm::folio_active&&>(duct));
        }
        context = {};
    }

    auto art_context_load(const art_context& context) noexcept -> void {
        const u64 cr2 = context.duct_phys & 0x7FFF'FFC0ULL;
        ctlreg::crwrite<2, 2>(cr2);
    }

    /// @brief Allocate an ASTE slot and populate it with a target ASCE.
    /// @param target_asce  The Address Space Control Element of the target domain.
    /// @param auth_index   Authorization Index (AX) for this ASTE.
    /// @return The ASTE index on success, or an error.
    [[nodiscard]] auto aste_alloc(
        u64 target_asce,
        u16 auth_index
    ) noexcept -> ::std::expected<u32, ::lib::kernel_error> {

        if (!g_initialized)
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::uninitialized));

        const zxfoundation::sync::irq_lock_guard _{g_aste_lock};

        const u32 idx = find_free_aste();
        if (idx >= MAX_ASTE_ENTRIES)
            return ::std::unexpected(
                ::lib::kernel_error::from_memory(::lib::memory_error::out_of_memory));

        auto* entry = aste_at(idx);
        entry->clear();
        entry->set_asce(target_asce);
        entry->set_authorization_index(auth_index);
        entry->set_astesn(1U); // matched by every ALE targeting this ASTE.
        // Entry is valid (I-bit cleared by clear()).

        g_aste_used[idx] = true;

        return idx;
    }

    /// @brief Free an ASTE slot, marking it as invalid.
    ///
    /// @param aste_index  The ASTE slot index to release.
    /// @return Success, or an error if the index is invalid.
    [[nodiscard]] auto aste_free(
        u32 aste_index
    ) noexcept -> ::std::expected<void, ::lib::kernel_error> {

        if (!g_initialized)
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::uninitialized));

        if (aste_index >= MAX_ASTE_ENTRIES)
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::out_of_range));

        const zxfoundation::sync::irq_lock_guard _{g_aste_lock};

        if (!g_aste_used[aste_index])
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::not_found));

        auto* entry = aste_at(aste_index);
        entry->set_invalid(true);
        g_aste_used[aste_index] = false;

        return {};
    }

    /// @brief Create an ALE that points to the given ASTE.
    /// @param aste_index  The ASTE slot that this ALE will reference.
    /// @return The ALET value (ALE slot index) on success, or an error.
    [[nodiscard]] auto ale_create(
        art_context& context, u32 aste_index
    ) noexcept -> ::std::expected<u32, ::lib::kernel_error> {

        if (!g_initialized)
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::uninitialized));

        if (context.duct_phys == 0U || context.ale_table_phys == 0U ||
            aste_index >= MAX_ASTE_ENTRIES)
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::out_of_range));

        const zxfoundation::sync::irq_lock_guard _{g_aste_lock};

        if (!g_aste_used[aste_index])
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::not_found));

        const u32 ale_idx = find_free_ale(context);
        if (ale_idx >= MAX_ALE_ENTRIES)
            return ::std::unexpected(
                ::lib::kernel_error::from_memory(::lib::memory_error::out_of_memory));

        // Compute the physical address of the target ASTE entry.
        const u64 aste_phys = g_aste_table_phys
                            + static_cast<u64>(aste_index) * sizeof(aste);

        auto* entry = ale_at(context, ale_idx);
        entry->clear();
        entry->set_aste_origin(aste_phys);
        entry->set_sequence(context.ale_sequence[ale_idx]);
        entry->set_aste_sequence(1U);

        return alet_make(static_cast<u16>(ale_idx), context.ale_sequence[ale_idx]);
    }

    /// @brief Invalidate an ALE and purge the ALB.
    /// @param alet  The ALET value (ALE slot index) to destroy.
    /// @return Success, or an error if the ALET is invalid.
    [[nodiscard]] auto ale_destroy(
        art_context& context, u32 alet
    ) noexcept -> ::std::expected<void, ::lib::kernel_error> {

        if (!g_initialized)
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::uninitialized));

        if ((alet & (ALET_RESERVED_MASK | ALET_PRIMARY_LIST)) != 0U)
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::out_of_range));

        const u32 ale_idx = alet_alen(alet);
        if (context.duct_phys == 0U || context.ale_table_phys == 0U ||
            ale_idx <= ALET_RESERVED_MAX || ale_idx >= MAX_ALE_ENTRIES)
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::out_of_range));

        const zxfoundation::sync::irq_lock_guard _{g_aste_lock};

        auto* entry = ale_at(context, ale_idx);
        if (entry->is_invalid() || entry->sequence() != alet_alesn(alet))
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::not_found));

        entry->set_invalid(true);
        context.ale_sequence[ale_idx] = ale_sequence_next(context.ale_sequence[ale_idx]);

        // Purge the ALB so the CPU does not use the now-invalid entry.
        ar::purge_alb();

        return {};
    }

    [[nodiscard]] auto aft_origin() noexcept -> u64 {
        return g_aft_phys >> 12U;
    }

    [[nodiscard]] auto aste_origin(u32 index) noexcept -> u64 {
        return g_aste_table_phys + static_cast<u64>(index) * 64U;
    }


    /// @brief Return a pointer to the ASTE at the given index.
    /// @param index  The ASTE slot index (0 to MAX_ASTE_ENTRIES-1).
    /// @return Pointer to the ASTE, or nullptr if the index is out of range
    ///         or the subsystem is not initialized.
    [[nodiscard]] auto aste_find(u32 index) noexcept -> aste* {

        if (!g_initialized || index >= MAX_ASTE_ENTRIES)
            return nullptr;

        return aste_at(index);
    }

} // namespace arch::s390x::cpu::aste
