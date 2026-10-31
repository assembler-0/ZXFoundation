/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/memory/rmap.zcomponent/core.cxx
/// @brief Reverse-map (rmap) implementation — implementation.

module zxfoundation.memory.rmap.core;
import zxfoundation.memory.rmap.types;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.vm.types;
import zxfoundation.memory.kmalloc;
import zxfoundation.memory.page_cache;
import zxfoundation.memory.hhdm;
import arch.s390x.mmu.core;
import arch.s390x.mmu.tlb;
import arch.s390x.cpu.skey;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.sys.zinit.core;
import zxfoundation.base.types;
import lib.error;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace zxfoundation::memory::rmap::detail {

    static constexpr u32 RMAP_CHUNK_ORDER = 3U;
    static constexpr u64 RMAP_ENTRIES_PER_CHUNK =
        (static_cast<u64>(PAGE_SIZE) << RMAP_CHUNK_ORDER) / sizeof(entry*);
    static constexpr u64 RMAP_MAX_CHUNKS =
        (pmm::MAX_FRAMES + RMAP_ENTRIES_PER_CHUNK - 1U) / RMAP_ENTRIES_PER_CHUNK;

    static std::array<entry**, RMAP_MAX_CHUNKS> g_rmap_chunks{};
    static u64              g_chunk_count{0};
    static u64              g_max_pfn{0};
    alignas(64) static std::array<sync::qspinlock::qspinlock<sync::lock_rank::rmap_stripe>, RMAP_LOCK_STRIPE> g_rmap_locks{};

    [[nodiscard]] static auto pfn_of(u64 phys) noexcept -> u64 {
        return phys >> 12;
    }

    /// @brief Stripe index for a PFN.
    [[nodiscard]] static auto stripe_of(u64 pfn) noexcept -> u32 {
        return static_cast<u32>(pfn & (RMAP_LOCK_STRIPE - 1U));
    }

    [[nodiscard]] static auto head_for_pfn(u64 pfn) noexcept -> entry*& {
        return g_rmap_chunks[pfn / RMAP_ENTRIES_PER_CHUNK]
            [pfn % RMAP_ENTRIES_PER_CHUNK];
    }

}

namespace zxfoundation::memory::rmap {

    using namespace detail;

    [[nodiscard]] auto release_slot_pin(
        vm::vm_page_slot& slot
    ) noexcept -> std::expected<bool, lib::kernel_error> {
        if (slot.state == vm::vm_page_state::empty ||
            slot.state == vm::vm_page_state::portal || slot.phys == 0ULL) {
            return false;
        }
        if (slot.cache_binding) {
            if (arch::s390x::cpu::skey::skey_is_changed(
                    arch::s390x::cpu::skey::iske(slot.phys))) {
                auto dirty = page_cache::mark_dirty_binding(slot.cache_binding);
                if (!dirty) return std::unexpected(dirty.error());
            }
            auto released = page_cache::release_binding(slot.cache_binding);
            if (!released) return std::unexpected(released.error());
            return false;
        }
        return pmm::frame_release_pin(slot.phys);
    }

    /// @brief Allocate the reverse-map heads for @p max_pfn frames.
    [[nodiscard]] auto zi_rmap_init(const sys::zinit::zi_desc&) noexcept
        -> sys::zinit::zi_result
    {
        u64 max_pfn = pmm::max_pfn();
        if (g_max_pfn != 0U) return {};
        if (max_pfn == 0U) return std::unexpected(
            lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        const u64 chunk_count =
            (max_pfn + RMAP_ENTRIES_PER_CHUNK - 1U) / RMAP_ENTRIES_PER_CHUNK;
        if (chunk_count > RMAP_MAX_CHUNKS) return std::unexpected(
            lib::kernel_error::from_memory(lib::memory_error::out_of_memory));

        for (u64 i = 0U; i < chunk_count; ++i) {
            auto alloc_r = pmm::alloc(pmm::allocation_request{
                .order = RMAP_CHUNK_ORDER,
                .mem_zone = pmm::zone::normal,
                .use = pmm::purpose::heap,
                .init = pmm::page_init::zeroed,
                .placement{},
            });
            if (!alloc_r) {
                for (u64 j = 0U; j < i; ++j) {
                    const u64 phys = hhdm::virt_to_phys(
                        reinterpret_cast<u64>(g_rmap_chunks[j]));
                    pmm::free(pmm::folio_active{
                        phys, RMAP_CHUNK_ORDER, pmm::purpose::heap, pmm::zone::normal});
                    g_rmap_chunks[j] = nullptr;
                }
                return std::unexpected(alloc_r.error());
            }
            g_rmap_chunks[i] = reinterpret_cast<entry**>(
                hhdm::phys_to_virt(alloc_r->phys_base));
        }
        g_chunk_count = chunk_count;
        g_max_pfn    = max_pfn;
        return {};
    }
    __zinit_dep__(mm, p80, zi_rmap_init, fatal, "zi_pmm_init")


    /// @brief Insert a reverse-map link: "slot @p slot_index in @p rgn
    ///        maps @p phys".
    [[nodiscard]] auto insert(vm::vm_region& rgn, u32 slot_index, u64 phys) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (g_chunk_count == 0U || phys == 0U) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::invalid_address));
        }
        const u64 pfn = pfn_of(phys);
        if (pfn >= g_max_pfn) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::invalid_address));
        }

        auto* node = static_cast<entry*>(
            zxfoundation::memory::kmalloc::kmalloc(sizeof(entry)));
        if (!node) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::out_of_memory));
        }
        node->region     = &rgn;
        node->slot_index = slot_index;
        node->next       = nullptr;

        const u32 s = stripe_of(pfn);
        const zxfoundation::sync::irq_lock_guard _{g_rmap_locks[s]};
        for (entry* cur = head_for_pfn(pfn); cur; cur = cur->next) {
            if (cur->region == &rgn && cur->slot_index == slot_index) {
                zxfoundation::memory::kmalloc::kfree(node);
                return {};
            }
        }
        node->next = head_for_pfn(pfn);
        head_for_pfn(pfn) = node;
        return {};
    }

    /// @brief Remove the link matching (@p rgn, @p slot_index, @p phys).
    auto remove(vm::vm_region& rgn, u32 slot_index, u64 phys) noexcept -> void {
        if (g_chunk_count == 0U || phys == 0U) return;
        const u64 pfn = pfn_of(phys);
        if (pfn >= g_max_pfn) return;

        const u32 s = stripe_of(pfn);
        const zxfoundation::sync::irq_lock_guard _{g_rmap_locks[s]};
        entry** pp = &head_for_pfn(pfn);
        while (*pp) {
            entry* cur = *pp;
            if (cur->region == &rgn && cur->slot_index == slot_index) {
                *pp = cur->next;
                zxfoundation::memory::kmalloc::kfree(cur);
                return;
            }
            pp = &cur->next;
        }
    }

    /// @brief Rebind an existing rmap entry after a VMA split without allocating.
    /// @return false when the old entry was absent or did not match.
    [[nodiscard]] auto rebind(
        vm::vm_region& from, u32 from_slot,
        vm::vm_region& to, u32 to_slot,
        u64 phys
    ) noexcept -> bool {
        if (g_chunk_count == 0U || phys == 0U) return false;
        const u64 pfn = pfn_of(phys);
        if (pfn >= g_max_pfn) return false;

        const u32 s = stripe_of(pfn);
        const zxfoundation::sync::irq_lock_guard _{g_rmap_locks[s]};
        for (entry* cur = head_for_pfn(pfn); cur; cur = cur->next) {
            if (cur->region == &from && cur->slot_index == from_slot) {
                cur->region = &to;
                cur->slot_index = to_slot;
                return true;
            }
        }
        return false;
    }

    /// @brief Walk every VMA slot that references @p phys.
    auto for_each_phys(u64 phys, visitor visit) noexcept -> u32 {
        if (g_chunk_count == 0U || phys == 0U || !visit) return 0U;
        const u64 pfn = pfn_of(phys);
        if (pfn >= g_max_pfn) return 0U;

        u32 visited = 0U;
        const u32 s = stripe_of(pfn);
        const zxfoundation::sync::irq_lock_guard _{g_rmap_locks[s]};
        for (entry* cur = head_for_pfn(pfn); cur; cur = cur->next) {
            if (!cur->region) continue;
            if (cur->slot_index >= cur->region->pages.count) continue;
            const auto& slot = cur->region->pages.slots[cur->slot_index];
            if (slot.state == vm::vm_page_state::empty) continue;
            if (slot.phys != phys) continue;
            ++visited;
            if (!visit(*cur->region, phys, slot.va_page, cur->slot_index))
                return visited;
        }
        return visited;
    }

    /// @brief Collect rmap identities without dereferencing VM page slots.
    auto collect_peers(u64 phys,
                        vm::vm_region& self, u32 self_slot,
                        peer_ref* out, u32 cap) noexcept -> u32 {
        if (g_chunk_count == 0U || phys == 0U || !out || cap == 0U) return 0U;
        const u64 pfn = pfn_of(phys);
        if (pfn >= g_max_pfn) return 0U;

        u32 n = 0U;
        const u32 s = stripe_of(pfn);
        const zxfoundation::sync::irq_lock_guard _{g_rmap_locks[s]};
        for (entry* cur = head_for_pfn(pfn); cur; cur = cur->next) {
            if (!cur->region) continue;
            if (cur->region == &self && cur->slot_index == self_slot) continue;
            out[n++] = peer_ref{ cur->region, cur->slot_index, 0U };
            if (n == cap) break;
        }
        return n;
    }

    /// @brief Invalidate every peer mapping of @p phys except @p self.
    /// @pre Caller holds self.owning_space->lock when self has an owning space.
    auto invalidate_peers(u64 phys,
                          vm::vm_region& self, u32 self_slot) noexcept
        -> std::expected<peer_invalidation_result, lib::kernel_error> {
        peer_invalidation_result result{};
        for (;;) {
            peer_ref peer{};
            const u32 n = collect_peers(phys, self, self_slot, &peer, 1U);
            if (n == 0U) break;

            auto* rgn = peer.region;
            if (!rgn) break;
            auto* space = rgn->owning_space;
            if (!space) {
                remove(*rgn, peer.slot_index, phys);
                continue;
            }

            if (space != self.owning_space) {
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::capability_invalid));
            }
            if (peer.slot_index >= rgn->pages.count) {
                remove(*rgn, peer.slot_index, phys);
                continue;
            }

            auto& slot = rgn->pages.slots[peer.slot_index];
            if (slot.state == vm::vm_page_state::empty || slot.phys != phys) {
                remove(*rgn, peer.slot_index, phys);
                continue;
            }
            discard_value arch::s390x::mmu::unmap_page_dispatch(
                space->asce_val, slot.va_page);
            arch::s390x::mmu::tlb_flush_asce(space->asce_val);
            auto released = release_slot_pin(slot);
            if (!released) return std::unexpected(released.error());
            if (rgn->resident.load(std::memory_order::relaxed) > 0U)
                rgn->resident.fetch_sub(1U, std::memory_order::relaxed);
            if (space->total_resident > 0U) --space->total_resident;
            remove(*rgn, peer.slot_index, phys);
            slot.clear();
            if (*released) ++result.frames_released;
            ++result.mappings_invalidated;
        }
        return result;
    }

    /// @brief Count how many VMA slots currently reference @p phys.
    [[nodiscard]] auto count_refs(u64 phys) noexcept -> u32 {
        if (g_chunk_count == 0U || phys == 0U) return 0U;
        const u64 pfn = pfn_of(phys);
        if (pfn >= g_max_pfn) return 0U;

        u32 count = 0U;
        const u32 s = stripe_of(pfn);
        const zxfoundation::sync::irq_lock_guard _{g_rmap_locks[s]};
        for (entry* cur = head_for_pfn(pfn); cur; cur = cur->next) {
            if (!cur->region) continue;
            if (cur->slot_index >= cur->region->pages.count) continue;
            const auto& slot = cur->region->pages.slots[cur->slot_index];
            if (slot.state == vm::vm_page_state::empty) continue;
            if (slot.phys != phys) continue;
            ++count;
        }
        return count;
    }

} // namespace zxfoundation::memory::rmap

