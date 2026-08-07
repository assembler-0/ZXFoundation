/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/aste.cxxm
/// @brief ASTE/ALE table manager

module arch.s390x.cpu.aste;
import arch.s390x.cpu.ar;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.ctlreg;
import zxfoundation.base.types;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm_types;
import zxfoundation.memory.hhdm;
import zxfoundation.locking.qspinlock;
import zxfoundation.locking.lock;
import arch.s390x.cpu.irq;
import std;
import :common;

using zxfoundation::memory::pmm::alloc;
using zxfoundation::memory::pmm::zone;
using zxfoundation::memory::pmm::purpose;
using zxfoundation::memory::pmm::page_init;
using zxfoundation::memory::hhdm::phys_to_virt;

namespace pmm = zxfoundation::memory::pmm;

namespace arch::s390x::cpu::aste {

    /// @brief Physical address of the ASTE table (MAX_ASTE_ENTRIES × 64 bytes).
    __internal__ u64 g_aste_table_phys{0};

    /// @brief Lock protecting all ASTE/ALE table mutations.
    __internal__ zxfoundation::locking::qspinlock::qspinlock g_aste_lock{};

    /// @brief Per-slot occupancy for the ASTE table (true = occupied).
    __internal__ std::array<bool, MAX_ASTE_ENTRIES> g_aste_used{};

    /// @brief Whether the subsystem has been initialized.
    __internal__ bool g_initialized{false};

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

} // namespace _internal


/// @name struct aste
/// @{
namespace arch::s390x::cpu::aste {

    /// @brief Test the I-bit (invalid) in word 0.
    [[nodiscard]] auto aste::is_invalid() const noexcept -> bool {
        return (words[0] & (1ULL << 63)) != 0;
    }

    /// @brief Set or clear the I-bit (invalid) in word 0.
    auto aste::set_invalid(bool inv) noexcept -> void {
        constexpr u64 i_mask = 1ULL << 63;
        if (inv)
            words[0] |= i_mask;
        else
            words[0] &= ~i_mask;
    }

    /// @brief Return the target address space ASCE (word 1).
    [[nodiscard]] auto aste::asce() const noexcept -> u64 {
        return words[1];
    }

    /// @brief Set the target address space ASCE (word 1).
    auto aste::set_asce(u64 val) noexcept -> void {
        words[1] = val;
    }

    /// @brief Return the Access-List Designation (word 2).
    [[nodiscard]] auto aste::ald() const noexcept -> u64 {
        return words[2] >> 32;
    }

    /// @brief Set the Access-List Designation (word 2).
    auto aste::set_ald(u64 val) noexcept -> void {
        words[2] = (words[2] & 0xFFFF'FFFFULL)
                 | ((val & 0xFFFF'FFFFULL) << 32);
    }

    /// @brief Return the Authorization Index (AX) from word 0 bits 32–47.
    [[nodiscard]] auto aste::authorization_index() const noexcept -> u16 {
        return static_cast<u16>((words[0] >> 16) & 0xFFFFU);
    }

    /// @brief Set the Authorization Index (AX) in word 0 bits 32–47.
    auto aste::set_authorization_index(u16 ax) noexcept -> void {
        words[0] = (words[0] & ~(0xFFFFULL << 16))
                  | (static_cast<u64>(ax) << 16);
    }

    /// @brief Return the raw 32-bit Linkage-Table Designation (bytes 24-27).
    [[nodiscard]] auto aste::ltd_raw() const noexcept -> u32 {
        return static_cast<u32>(words[3] & 0xFFFFFFFFU);
    }

    /// @brief Set the raw 32-bit Linkage-Table Designation (bytes 24-27).
    auto aste::set_ltd_raw(u32 ltd) noexcept -> void {
        words[3] = (words[3] & ~0xFFFFFFFFULL)
                  | (static_cast<u64>(ltd) << 0);
    }

    /// @brief Test the subsystem-linkage control bit (V, LTD bit 0).
    /// Must be 1 for PC/PT instructions to proceed.
    [[nodiscard]] auto aste::ltd_valid() const noexcept -> bool {
        return (ltd_raw() >> 31) & 1U;
    }

    /// @brief Set or clear the V bit in the LTD.
    auto aste::set_ltd_valid(bool v) noexcept -> void {
        u32 ltd = ltd_raw();
        if (v)
            ltd |= (1U << 31);
        else
            ltd &= ~(1U << 31);
        set_ltd_raw(ltd);
    }

    /// @brief Return the linkage-table origin as a 31-bit real address.
    /// LTD bits 1-24 with 7 zeros appended on the right.
    [[nodiscard]] auto aste::ltd_origin() const noexcept -> u64 {
        return static_cast<u64>(ltd_raw() & 0x7FFFFF80U);
    }

    /// @brief Set the linkage-table origin from a 128-byte-aligned real address.
    /// @pre @p addr must be 128-byte aligned (low 7 bits zero).
    auto aste::set_ltd_origin(u64 addr) noexcept -> void {
        u32 ltd = ltd_raw();
        // Preserve V (bit 31) and LTL (bits 0-6)
        ltd = (ltd & 0x8000007FU) | (static_cast<u32>(addr) & 0x7FFFFF80U);
        set_ltd_raw(ltd);
    }

    /// @brief Return the linkage-table length (LTL, LTD bits 25-31, 7 bits).
    /// Actual table length = (LTL + 1) × 128 bytes.
    [[nodiscard]] auto aste::ltd_length() const noexcept -> u8 {
        return static_cast<u8>(ltd_raw() & 0x7FU);
    }

    /// @brief Set the linkage-table length (LTL).
    auto aste::set_ltd_length(u8 ltl) noexcept -> void {
        u32 ltd = ltd_raw();
        ltd = (ltd & ~0x7FU) | (static_cast<u32>(ltl) & 0x7FU);
        set_ltd_raw(ltd);
    }

    /// @brief Zero all words.
    auto aste::clear() noexcept -> void {
       ::std::fill_n(words, sizeof(words) / sizeof(words[0]), u64{0});
    }

}
/// @}

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
    auto aste_subsystem_init() noexcept -> void {

        constexpr u64 aste_table_size = MAX_ASTE_ENTRIES * sizeof(aste);
        constexpr u32 aste_pages = pages_for(aste_table_size);
        constexpr u32 aste_order = order_for_pages(aste_pages);

        auto aste_result = alloc(aste_order, zone::dma31,
                                 purpose::page_table, page_init::zeroed);
        if (!aste_result) {
            return;
        }
        g_aste_table_phys = aste_result.value().phys_base;

        for (u32 i = 0; i < MAX_ASTE_ENTRIES; ++i) {
            aste_at(i)->set_invalid(true);
            g_aste_used[i] = false;
        }

        g_initialized = true;
    }

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
        // DUALD is the 32-bit ALD at byte 16: ALO is the table origin in
        // 128-byte units and ALL admits entries 0 through 15.
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
        ctlreg::ctl_load<2, 2>(cr2);
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

        const zxfoundation::locking::irq_lock_guard guard{g_aste_lock};

        const u32 idx = find_free_aste();
        if (idx >= MAX_ASTE_ENTRIES)
            return ::std::unexpected(
                ::lib::kernel_error::from_memory(::lib::memory_error::out_of_memory));

        auto* entry = aste_at(idx);
        entry->clear();
        entry->set_asce(target_asce);
        entry->set_authorization_index(auth_index);
        entry->words[2] = 1U; // ASTESN; matched by every ALE targeting this ASTE.
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

        const zxfoundation::locking::irq_lock_guard guard{g_aste_lock};

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

        const zxfoundation::locking::irq_lock_guard guard{g_aste_lock};

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
    ///
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

        const zxfoundation::locking::irq_lock_guard guard{g_aste_lock};

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
