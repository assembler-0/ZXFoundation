/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/mmu/core.cxx
/// @brief MMU engine implementation — DAT table walk/map internals.

module arch.s390x.mmu.core;
import arch.s390x.mmu.types;
import arch.s390x.mmu.consts;
import arch.s390x.mmu.tlb;
import arch.s390x.cpu.features;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.ctlreg;
import zxfoundation.base.types;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.hhdm;
import zxfoundation.base.transaction;
import lib.error;
import lib.stl;

namespace arch::s390x::mmu::detail {

    using namespace zxfoundation::memory;

    bool g_has_edat1{false};
    bool g_has_edat2{false};

    inline bool g_initialized{false};

    /// @brief Build the default PMM request used for DAT table pages.
    [[nodiscard]] auto default_table_request() noexcept -> pmm::allocation_request {
        return pmm::allocation_request{
            .order = CRST_ALLOC_ORDER,
            .mem_zone = pmm::zone::normal,
            .use = pmm::purpose::page_table,
            .init = pmm::page_init::uninitialized,
            .placement{},
        };
    }

    /// @brief Allocate one DAT table using a caller-supplied placement policy.
    [[nodiscard]] auto alloc_table_with_policy(
        dat_level lvl,
        const pmm::allocation_request& policy
    ) noexcept -> ::std::expected<u64, ::lib::kernel_error> {
        pmm::allocation_request request = policy;
        request.order = (lvl == dat_level::page) ? PTE_ALLOC_ORDER : CRST_ALLOC_ORDER;
        request.mem_zone = pmm::zone::normal;
        request.use = pmm::purpose::page_table;

        auto result = pmm::alloc(request);
        if (!result) return ::std::unexpected(result.error());
        return result->phys_base;
    }

    /// @brief Fill a freshly-allocated table with empty (invalid) entries.
    [[nodiscard]] auto init_table(dat_level lvl, u64 phys) noexcept -> ::std::expected<void, ::lib::kernel_error> {
        const u64 table_bytes = (lvl == dat_level::page)
            ? static_cast<u64>(PAGE_ENTRIES) * sizeof(u64)
            : static_cast<u64>(CRST_ENTRIES) * sizeof(u64);
        if ((phys & (static_cast<u64>(PAGE_SIZE) - 1U)) != 0U) {
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::code::alignment_error));
        }
        if (!hhdm::phys_range_is_mapped(phys, table_bytes)) {
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::code::hhdm_init_failed));
        }

        volatile u64* table = reinterpret_cast<volatile u64*>(hhdm::phys_to_virt(phys));
        if (lvl == dat_level::page) {
            for (u32 i = 0; i < PAGE_ENTRIES; ++i)
                table[i] = PTE_EMPTY;
        } else {
            const u64 empty = rste_empty_rt(lvl);
            for (u32 i = 0; i < CRST_ENTRIES; ++i)
                table[i] = empty;
        }

        return {};
    }

    /// @brief Fail-closed check that a DAT table physical page is backed by the
    ///        HHDM before it is read or written through hhdm::phys_to_virt().
    /// @param[in] phys  Physical base of the table (CRST = 16 KiB, page = 2 KiB).
    /// @param[in] lvl   Level whose table size is being validated.
    [[nodiscard]] auto table_is_mapped(u64 phys, dat_level lvl) noexcept -> bool {
        const u64 table_bytes = (lvl == dat_level::page)
            ? static_cast<u64>(PAGE_ENTRIES) * sizeof(u64)
            : static_cast<u64>(CRST_ENTRIES) * sizeof(u64);
        if ((phys & (static_cast<u64>(PAGE_SIZE) - 1U)) != 0U) return false;
        return hhdm::phys_range_is_mapped(phys, table_bytes);
    }

    /// @brief Record of one intermediate table allocated during a walk.
    struct alloc_record {
        u64 phys{0};
        u32 order{0};
    };

    /// @brief Stack of allocations made during a single map_page walk.
    ///        Maximum depth: 4 (R2, R3, Seg, PTE from an R1 root).
    struct alloc_stack {
        alloc_record entries[4]{};
        u32 count{0};

        auto push(u64 phys, u32 order) noexcept -> void {
            if (count < 4U)
                entries[count++] = {phys, order};
        }

        auto rollback() noexcept -> void {
            for (u32 i = 0; i < count; ++i) {
                pmm::folio_active f{entries[i].phys, entries[i].order,
                                   pmm::purpose::page_table, pmm::zone::normal};
                pmm::free(::std::move(f));
            }
            count = 0;
        }
    };

    /// @brief Per-level state captured during Phase 1.
    struct level_state {
        u64  table_phys{0};     ///< Physical address of the table at this level.
        u32  index{0};          ///< Entry index within the table.
        u64  alloc_phys{0};     ///< 0 if child existed; else newly allocated child phys.
    };

    /// @brief Map a single 4 KiB page using the two-phase protocol.
    /// @return  On success, empty expected.
    ///          On failure, kernel_error; NO side effects.
    auto map_page_impl(
        u64       root_phys,
        dat_level root_level,
        u64       virt,
        u64       phys,
        page_prot prot,
        const pmm::allocation_request& table_policy
    ) noexcept -> ::std::expected<void, ::lib::kernel_error> {
        level_state states[4]{};
        alloc_stack allocs{};
        u32 depth = 0;
        u64 current = root_phys;

        for (auto lvl = root_level; lvl != dat_level::page;
             lvl = next_level(lvl), ++depth) {
            const u32 idx = virt_index_rt(lvl, virt);
            states[depth].table_phys = current;
            states[depth].index      = idx;
            states[depth].alloc_phys = 0;

            // Fail closed if this table's physical page is not HHDM-backed.
            if (!table_is_mapped(current, lvl)) [[unlikely]] {
                allocs.rollback();
                return ::std::unexpected(::lib::kernel_error::from_memory(
                    ::lib::memory_error::code::hhdm_init_failed));
            }

            auto* table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
            const u64 entry = table[idx];
            const dat_level child_lvl = next_level(lvl);

            if (rste_is_invalid(entry)) {
                // Need to allocate the child table.
                const u32 order = (child_lvl == dat_level::page)
                    ? PTE_ALLOC_ORDER : CRST_ALLOC_ORDER;
                auto result = alloc_table_with_policy(child_lvl, table_policy);
                if (!result) [[unlikely]] {
                    allocs.rollback();
                    return ::std::unexpected(result.error());
                }
                const u64 child_phys = *result;
                auto init_result = init_table(child_lvl, child_phys);
                if (!init_result) [[unlikely]] {
                    pmm::folio_active f{child_phys, order, pmm::purpose::page_table, pmm::zone::normal};
                    pmm::free(::std::move(f));
                    allocs.rollback();
                    return ::std::unexpected(init_result.error());
                }
                allocs.push(child_phys, order);
                states[depth].alloc_phys = child_phys;
                current = child_phys;
            } else {
                // Segment large page blocks 4 KiB mapping beneath it.
                if (lvl == dat_level::segment && seg_is_large(entry))
                    [[unlikely]] {
                    allocs.rollback();
                    return ::std::unexpected(::lib::kernel_error::from_memory(
                        ::lib::memory_error::code::region_overlap));
                }
                // Region-3 large page blocks everything beneath it.
                if (lvl == dat_level::region_3 && r3_is_large(entry))
                    [[unlikely]] {
                    allocs.rollback();
                    return ::std::unexpected(::lib::kernel_error::from_memory(
                        ::lib::memory_error::code::region_overlap));
                }
                current = rste_origin(entry);
            }
        }

        // Check PTE slot.
        if (!table_is_mapped(current, dat_level::page)) [[unlikely]] {
            allocs.rollback();
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::code::hhdm_init_failed));
        }
        auto* pte_table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
        const u32 pte_idx = virt_index_rt(dat_level::page, virt);
        if (!pte_is_invalid(pte_table[pte_idx])) [[unlikely]] {
            allocs.rollback();
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::code::region_overlap));
        }

        u64 walk = root_phys;
        for (u32 i = 0; i < depth; ++i) {
            if (states[i].alloc_phys != 0) {
                auto* table = reinterpret_cast<u64*>(
                    hhdm::phys_to_virt(states[i].table_phys));
                const dat_level lvl = level_at_depth(root_level, i);
                table[states[i].index] = rste_make_rt(lvl, states[i].alloc_phys);
                walk = states[i].alloc_phys;
            } else {
                auto* table = reinterpret_cast<u64*>(
                    hhdm::phys_to_virt(states[i].table_phys));
                walk = rste_origin(table[states[i].index]);
            }
        }

        // Install the PTE.
        auto* final_pte_table = reinterpret_cast<u64*>(hhdm::phys_to_virt(walk));
        final_pte_table[virt_index_rt(dat_level::page, virt)] =
            pte_make(phys, prot);

        return {};
    }

    [[nodiscard]] auto table_is_empty(dat_level level, u64 phys) noexcept -> bool {
        auto* table = reinterpret_cast<const u64*>(hhdm::phys_to_virt(phys));
        const u32 entries = level == dat_level::page ? PAGE_ENTRIES : CRST_ENTRIES;
        for (u32 i = 0; i < entries; ++i) {
            const bool invalid = level == dat_level::page
                ? pte_is_invalid(table[i])
                : rste_is_invalid(table[i]);
            if (!invalid) return false;
        }
        return true;
    }

    auto table_free_level(dat_level level, u64 phys) noexcept -> void {
        const u32 order = level == dat_level::page
            ? PTE_ALLOC_ORDER : CRST_ALLOC_ORDER;
        pmm::folio_active f{
            phys, order, pmm::purpose::page_table, pmm::zone::normal};
        pmm::free(::std::move(f));
    }

    /// @brief Unmap whatever exists at @p virt (4 KiB, 1 MiB, or 2 GiB).
    /// @return Size of the mapping removed, or kernel_error if nothing was mapped.
    auto unmap_at_impl(
        u64       root_phys,
        dat_level root_level,
        u64       virt
    ) noexcept -> ::std::expected<u64, ::lib::kernel_error> {
        u64 tables[4]{};
        u32 indices[4]{};
        u32 depth = 0;
        u64 current = root_phys;
        dat_level current_level = root_level;
        u64 unmapped_size = PAGE_SIZE_4K;

        for (; current_level != dat_level::page;
             current_level = next_level(current_level)) {
            auto* table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
            const u32 index = virt_index_rt(current_level, virt);
            const u64 entry = table[index];
            if (rste_is_invalid(entry)) {
                return ::std::unexpected(::lib::kernel_error::from_memory(
                    ::lib::memory_error::code::invalid_address));
            }

            if ((current_level == dat_level::segment && seg_is_large(entry)) ||
                (current_level == dat_level::region_3 && r3_is_large(entry))) {
                table[index] = rste_empty_rt(current_level);
                unmapped_size = current_level == dat_level::segment
                    ? SEGMENT_SIZE : REGION3_SIZE;
                break;
            }

            tables[depth] = current;
            indices[depth] = index;
            ++depth;
            current = rste_origin(entry);
        }

        if (current_level == dat_level::page) {
            auto* table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
            const u32 index = virt_index_rt(dat_level::page, virt);
            if (pte_is_invalid(table[index])) {
                return ::std::unexpected(::lib::kernel_error::from_memory(
                    ::lib::memory_error::code::invalid_address));
            }
            tlb_invalidate_pte(current, virt);
            table[index] = PTE_EMPTY;
        }

        for (u32 step = depth; step > 0; --step) {
            const u32 parent_step = step - 1U;
            if (!table_is_empty(current_level, current)) break;

            const dat_level parent_level = level_at_depth(root_level, parent_step);
            auto* parent = reinterpret_cast<u64*>(
                hhdm::phys_to_virt(tables[parent_step]));
            parent[indices[parent_step]] = rste_empty_rt(parent_level);
            table_free_level(current_level, current);
            current = tables[parent_step];
            current_level = parent_level;
        }

        return unmapped_size;
    }

    auto walk_impl(
        u64       root_phys,
        dat_level root_level,
        u64       virt
    ) noexcept -> ::std::expected<u64, ::lib::kernel_error> {
        u64 current = root_phys;

        for (auto lvl = root_level; lvl != dat_level::page;
             lvl = next_level(lvl)) {
            auto* table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
            const u32 idx = virt_index_rt(lvl, virt);
            const u64 entry = table[idx];

            if (rste_is_invalid(entry))
                return ::std::unexpected(::lib::kernel_error::from_memory(
                    ::lib::memory_error::code::invalid_address));

            if (lvl == dat_level::segment && seg_is_large(entry)) {
                const u64 base = entry & SEG_LARGE_ORIGIN;
                return base | (virt & (SEGMENT_SIZE - 1));
            }
            if (lvl == dat_level::region_3 && r3_is_large(entry)) {
                const u64 base = entry & R3_LARGE_ORIGIN;
                return base | (virt & (REGION3_SIZE - 1));
            }
            current = rste_origin(entry);
        }

        auto* pte_table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
        const u32 pte_idx = virt_index_rt(dat_level::page, virt);
        const u64 pte_val = pte_table[pte_idx];
        if (pte_is_invalid(pte_val))
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::code::invalid_address));
        return pte_frame(pte_val) | (virt & (PAGE_SIZE_4K - 1));
    }

    auto protect_impl(
        u64       root_phys,
        dat_level root_level,
        u64       virt,
        page_prot new_prot
    ) noexcept -> ::std::expected<void, ::lib::kernel_error> {
        u64 current = root_phys;

        for (auto lvl = root_level; lvl != dat_level::page;
             lvl = next_level(lvl)) {
            auto* table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
            const u32 idx = virt_index_rt(lvl, virt);
            const u64 entry = table[idx];

            if (rste_is_invalid(entry))
                return ::std::unexpected(::lib::kernel_error::from_memory(
                    ::lib::memory_error::code::invalid_address));

            // Large pages: re-protect in place.
            if (lvl == dat_level::segment && seg_is_large(entry)) {
                const u64 base = entry & SEG_LARGE_ORIGIN;
                table[idx] = seg_large_make(base, new_prot);
                tlb_flush_local();
                return {};
            }
            if (lvl == dat_level::region_3 && r3_is_large(entry)) {
                const u64 base = entry & R3_LARGE_ORIGIN;
                table[idx] = r3_large_make(base, new_prot);
                tlb_flush_local();
                return {};
            }
            current = rste_origin(entry);
        }

        auto* pte_table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
        const u32 pte_idx = virt_index_rt(dat_level::page, virt);
        const u64 old_pte = pte_table[pte_idx];

        if (pte_is_invalid(old_pte))
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::code::invalid_address));

        const u64 frame = pte_frame(old_pte);

        // IPTE: atomically invalidate + TLB flush.
        tlb_invalidate_pte(current, virt);
        // Re-install with new protection.
        pte_table[pte_idx] = pte_make(frame, new_prot);
        tlb_flush_local();
        return {};
    }

    /// @brief Map a 1 MiB large page (EDAT-1) via segment entry.
    auto map_large_1m_impl(
        u64       root_phys,
        dat_level root_level,
        u64       virt,
        u64       phys,
        page_prot prot
    ) noexcept -> ::std::expected<void, ::lib::kernel_error> {
        // Walk from root to segment level, allocating intermediates.
        alloc_stack allocs{};
        level_state states[3]{};   // At most 3 levels above segment (R1,R2,R3).
        u32 depth = 0;
        u64 current = root_phys;

        for (auto lvl = root_level; lvl != dat_level::segment;
             lvl = next_level(lvl), ++depth) {
            const u32 idx = virt_index_rt(lvl, virt);
            states[depth] = {current, idx, 0};

            auto* table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
            const u64 entry = table[idx];
            const dat_level child_lvl = next_level(lvl);

            if (rste_is_invalid(entry)) {
                const u32 order = CRST_ALLOC_ORDER;
                auto result = pmm::alloc(
                    order, pmm::zone::normal, pmm::purpose::page_table);
                if (!result) [[unlikely]] {
                    allocs.rollback();
                    return ::std::unexpected(result.error());
                }
                const u64 child_phys = result->phys_base;
                auto init_result = init_table(child_lvl, child_phys);
                if (!init_result) [[unlikely]] {
                    pmm::folio_active f{child_phys, order, pmm::purpose::page_table, pmm::zone::normal};
                    pmm::free(::std::move(f));
                    allocs.rollback();
                    return ::std::unexpected(init_result.error());
                }
                allocs.push(child_phys, order);
                states[depth].alloc_phys = child_phys;
                current = child_phys;
            } else {
                if (lvl == dat_level::region_3 && r3_is_large(entry))
                    [[unlikely]] {
                    allocs.rollback();
                    return ::std::unexpected(::lib::kernel_error::from_memory(
                        ::lib::memory_error::code::region_overlap));
                }
                current = rste_origin(entry);
            }
        }

        // Now current = segment table phys.  Check the segment entry.
        auto* seg_table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
        const u32 seg_idx = virt_index_rt(dat_level::segment, virt);
        if (!rste_is_invalid(seg_table[seg_idx])) [[unlikely]] {
            allocs.rollback();
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::code::region_overlap));
        }

        u64 walk = root_phys;
        for (u32 i = 0; i < depth; ++i) {
            if (states[i].alloc_phys != 0) {
                auto* table = reinterpret_cast<u64*>(
                    hhdm::phys_to_virt(states[i].table_phys));
                table[states[i].index] = rste_make_rt(
                    level_at_depth(root_level, i), states[i].alloc_phys);
                walk = states[i].alloc_phys;
            } else {
                auto* table = reinterpret_cast<u64*>(
                    hhdm::phys_to_virt(states[i].table_phys));
                walk = rste_origin(table[states[i].index]);
            }
        }

        // Install the large segment entry.
        auto* final_seg = reinterpret_cast<u64*>(hhdm::phys_to_virt(walk));
        final_seg[virt_index_rt(dat_level::segment, virt)] =
            seg_large_make(phys, prot);
        return {};
    }

    /// @brief Map a 2 GiB large page (EDAT-2) via region-3 entry.
    auto map_large_2g_impl(
        u64       root_phys,
        dat_level root_level,
        u64       virt,
        u64       phys,
        page_prot prot
    ) noexcept -> ::std::expected<void, ::lib::kernel_error> {
        alloc_stack allocs{};
        level_state states[2]{};   // At most R1,R2 above R3.
        u32 depth = 0;
        u64 current = root_phys;

        for (auto lvl = root_level; lvl != dat_level::region_3;
             lvl = next_level(lvl), ++depth) {
            const u32 idx = virt_index_rt(lvl, virt);
            states[depth] = {current, idx, 0};

            auto* table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
            const u64 entry = table[idx];
            const dat_level child_lvl = next_level(lvl);

            if (rste_is_invalid(entry)) {
                auto result = pmm::alloc(
                    CRST_ALLOC_ORDER, pmm::zone::normal, pmm::purpose::page_table);
                if (!result) [[unlikely]] {
                    allocs.rollback();
                    return ::std::unexpected(result.error());
                }
                const u64 child_phys = result->phys_base;
                auto init_result = init_table(child_lvl, child_phys);
                if (!init_result) [[unlikely]] {
                    pmm::folio_active f{child_phys, CRST_ALLOC_ORDER, pmm::purpose::page_table, pmm::zone::normal};
                    pmm::free(::std::move(f));
                    allocs.rollback();
                    return ::std::unexpected(init_result.error());
                }
                allocs.push(child_phys, CRST_ALLOC_ORDER);
                states[depth].alloc_phys = child_phys;
                current = child_phys;
            } else {
                current = rste_origin(entry);
            }
        }

        // current = region-3 table phys.
        auto* r3_table = reinterpret_cast<u64*>(hhdm::phys_to_virt(current));
        const u32 r3_idx = virt_index_rt(dat_level::region_3, virt);
        if (!rste_is_invalid(r3_table[r3_idx])) [[unlikely]] {
            allocs.rollback();
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::code::region_overlap));
        }

        u64 walk = root_phys;
        for (u32 i = 0; i < depth; ++i) {
            if (states[i].alloc_phys != 0) {
                auto* table = reinterpret_cast<u64*>(
                    hhdm::phys_to_virt(states[i].table_phys));
                table[states[i].index] = rste_make_rt(
                    level_at_depth(root_level, i), states[i].alloc_phys);
                walk = states[i].alloc_phys;
            } else {
                auto* table = reinterpret_cast<u64*>(
                    hhdm::phys_to_virt(states[i].table_phys));
                walk = rste_origin(table[states[i].index]);
            }
        }

        auto* final_r3 = reinterpret_cast<u64*>(hhdm::phys_to_virt(walk));
        final_r3[virt_index_rt(dat_level::region_3, virt)] =
            r3_large_make(phys, prot);
        return {};
    }
    /// @brief Maximum depth of recursive DAT table walk for pre-init validation.
    inline constexpr u32 PREINIT_MAX_TABLES = 2048;

    u64 g_preinit_table_phys[PREINIT_MAX_TABLES]{};
    u32 g_preinit_table_count{0};

    /// @brief Walk one DAT table level and recurse into child tables.
    [[nodiscard]] auto validate_table_walk(
        u64       table_phys,
        dat_level lvl,
        u64       hhdm_start,
        u64       hhdm_end
    ) noexcept -> ::std::expected<void, ::lib::kernel_error> {
        if (table_phys < hhdm_start || table_phys >= hhdm_end) {
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::code::hhdm_init_failed));
        }

        if (g_preinit_table_count >= PREINIT_MAX_TABLES) {
            return ::std::unexpected(::lib::kernel_error::from_memory(
                ::lib::memory_error::code::out_of_memory));
        }

        const u32 this_idx = g_preinit_table_count++;
        g_preinit_table_phys[this_idx] = table_phys;

        const u64 table_virt = zxfoundation::memory::hhdm::phys_to_virt(table_phys);
        const u32 entries = (lvl == dat_level::page) ? PAGE_ENTRIES : CRST_ENTRIES;
        const auto* table = reinterpret_cast<const u64*>(table_virt);

        for (u32 i = 0; i < entries; ++i) {
            const u64 entry = table[i];
            if (rste_is_invalid(entry))
                continue;

            if (lvl == dat_level::page)
                continue;

            if (lvl == dat_level::region_3 && r3_is_large(entry))
                continue;

            if (lvl == dat_level::segment && seg_is_large(entry))
                continue;

            const u64 child_phys = rste_origin(entry);
            const dat_level child_lvl = next_level(lvl);

            auto result = validate_table_walk(child_phys, child_lvl, hhdm_start, hhdm_end);
            if (!result)
                return result;
        }

        return {};
    }

} // namespace arch::s390x::mmu::detail

namespace arch::s390x::mmu {

    /// @brief Initialize the MMU engine.  Must be called after PMM init
    ///        and arch::s390x::cpu::features::init().
    auto init() noexcept -> void {
        using namespace detail;
        namespace features = arch::s390x::cpu::features;
        if (g_initialized) return;
        g_has_edat1 = features::has(features::facility::edat1);
        g_has_edat2 = features::has(features::facility::edat2);
        g_initialized = true;
    }

    /// @brief Query EDAT-1 (1 MiB large page) availability.
    [[nodiscard]] auto has_edat1() noexcept -> bool {
        return detail::g_has_edat1;
    }

    /// @brief Query EDAT-2 (2 GiB large page) availability.
    [[nodiscard]] auto has_edat2() noexcept -> bool {
        return detail::g_has_edat2;
    }

    /// @brief Allocate and initialize a page table.
    [[nodiscard]] auto pte_table_alloc() noexcept
        -> ::std::expected<dat_table<dat_level::page>, ::lib::kernel_error>
    {
        auto result = zxfoundation::memory::pmm::alloc(
            PTE_ALLOC_ORDER,
            zxfoundation::memory::pmm::zone::normal,
            zxfoundation::memory::pmm::purpose::page_table);
        if (!result) [[unlikely]]
            return ::std::unexpected(result.error());
        const u64 phys = result->phys_base;
        auto init_result = detail::init_table(dat_level::page, phys);
        if (!init_result) [[unlikely]] {
            zxfoundation::memory::pmm::folio_active f{
                phys,
                PTE_ALLOC_ORDER,
                zxfoundation::memory::pmm::purpose::page_table,
                zxfoundation::memory::pmm::zone::normal};
            zxfoundation::memory::pmm::free(::std::move(f));
            return ::std::unexpected(init_result.error());
        }
        return dat_table<dat_level::page>(phys);
    }

    /// @brief Runtime-dispatched unmap keyed on an ASCE value.
    /// @param[in] asce_val ASCE value (root table + type + length).
    /// @param[in] virt     Virtual address whose PTE to invalidate.
    /// @return The physical address of the unmapped frame, or
    ///         kernel_error (not_present if the address was already
    ///         unmapped, invalid_address if the root level is bogus).
    [[nodiscard]] auto unmap_page_dispatch(
        u64 asce_val, u64 virt
    ) noexcept -> ::std::expected<u64, ::lib::kernel_error> {
        const u64 root_phys = asce_val & ASCE_ORIGIN_MASK;
        const u64 dt        = asce_val & ASCE_TYPE_MASK;

        if (dt == ASCE_TYPE_R1) {
            const dat_table<dat_level::region_1> root{root_phys};
            return unmap_page(root, virt);
        }
        if (dt == ASCE_TYPE_R2) {
            const dat_table<dat_level::region_2> root{root_phys};
            return unmap_page(root, virt);
        }
        if (dt == ASCE_TYPE_R3) {
            const dat_table<dat_level::region_3> root{root_phys};
            return unmap_page(root, virt);
        }
        (void)root_phys; (void)virt; // DT was not r1/r2/r3
        return ::std::unexpected(::lib::kernel_error::from_memory(
            ::lib::memory_error::code::invalid_address));
    }

    /// @brief Advance the cursor after a successful mapping.
    auto mmu_transaction::advance(u64 bytes) noexcept -> void {
        range_cursor_ += bytes;
    }

    /// @brief Commit the transaction and flush TLB.
    auto mmu_transaction::commit_and_flush() noexcept -> void {
        if (range_cursor_ > range_start_)
            tlb_flush_asce(asce_val_);
        this->commit();
    }

    /// @brief Rollback: unmap all successfully-mapped entries, then flush.
    auto mmu_transaction::on_rollback() const noexcept -> void {
        u64 va = range_start_;
        while (va < range_cursor_) {
            auto r = detail::unmap_at_impl(
                root_phys_, root_level_, va);
            if (r) {
                va += *r;
            } else {
                va += PAGE_SIZE_4K;
            }
        }
        if (range_cursor_ > range_start_)
            tlb_flush_asce(asce_val_);
    }

} // namespace arch::s390x::mmu