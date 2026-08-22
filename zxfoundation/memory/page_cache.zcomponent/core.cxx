/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/memory/page_cache.zcomponent/core.cxx
/// @brief Bounded file-backed page cache with explicit residency and mapping pins — implementation.

module zxfoundation.memory.page_cache.core;
import zxfoundation.memory.page_cache.types;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.hhdm;
import zxfoundation.memory.kmalloc;
import zxfoundation.memory.objects;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.kobject.types;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.memory_contract;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import arch.s390x.cpu.skey;
import zxfoundation.base.types;
import lib.error;
import std;


namespace zxfoundation::memory::page_cache::detail {

    cache_entry* g_entries{nullptr};
    u32* g_key_heads{nullptr};
    u32* g_phys_heads{nullptr};
    u32 g_free_head{PAGE_CACHE_SLOT_INVALID};
    u32 g_resident_count{0U};
    alignas(4) zxfoundation::sync::qspinlock::qspinlock g_lock{};
    read_page_fn g_reader{nullptr};
    write_page_fn g_writer{nullptr};

    [[nodiscard]] constexpr auto hash_mix(u64 value) noexcept -> u64 {
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdULL;
        value ^= value >> 33;
        return value;
    }

    [[nodiscard]] auto key_bucket(void* node, u32 index) noexcept -> u32 {
        const u64 key = hash_mix(static_cast<u64>(reinterpret_cast<usize>(node))
            ^ (static_cast<u64>(index) << 32));
        return static_cast<u32>(key & (PAGE_CACHE_INDEX_SLOTS - 1U));
    }

    [[nodiscard]] auto phys_bucket(u64 phys) noexcept -> u32 {
        return static_cast<u32>(hash_mix(phys >> PAGE_SHIFT)
            & (PAGE_CACHE_INDEX_SLOTS - 1U));
    }

    [[nodiscard]] auto index_of(const cache_entry& entry) noexcept -> u32 {
        return static_cast<u32>(&entry - g_entries);
    }

    [[nodiscard]] auto next_generation(u32 generation) noexcept -> u32 {
        ++generation;
        return generation == 0U ? 1U : generation;
    }

    auto reset_entry(cache_entry& entry) noexcept -> void {
        const u32 generation = next_generation(entry.generation);
        entry = {};
        entry.generation = generation;
        entry.key_next = PAGE_CACHE_SLOT_INVALID;
        entry.phys_next = PAGE_CACHE_SLOT_INVALID;
        entry.free_next = PAGE_CACHE_SLOT_INVALID;
    }

    [[nodiscard]] auto ensure_entries_locked() noexcept -> bool {
        if (g_entries) return true;

        auto* entries = static_cast<cache_entry*>(
            kmalloc::kmalloc(sizeof(cache_entry) * PAGE_CACHE_SLOTS));
        auto* key_heads = static_cast<u32*>(
            kmalloc::kmalloc(sizeof(u32) * PAGE_CACHE_INDEX_SLOTS));
        auto* phys_heads = static_cast<u32*>(
            kmalloc::kmalloc(sizeof(u32) * PAGE_CACHE_INDEX_SLOTS));
        if (!entries || !key_heads || !phys_heads) {
            if (entries) kmalloc::kfree(entries);
            if (key_heads) kmalloc::kfree(key_heads);
            if (phys_heads) kmalloc::kfree(phys_heads);
            return false;
        }

        for (u32 i = 0U; i < PAGE_CACHE_INDEX_SLOTS; ++i) {
            key_heads[i] = PAGE_CACHE_SLOT_INVALID;
            phys_heads[i] = PAGE_CACHE_SLOT_INVALID;
        }
        for (u32 i = 0U; i < PAGE_CACHE_SLOTS; ++i) {
            entries[i] = {};
            entries[i].state = cache_entry_state::empty;
            entries[i].key_next = PAGE_CACHE_SLOT_INVALID;
            entries[i].phys_next = PAGE_CACHE_SLOT_INVALID;
            entries[i].free_next = i + 1U < PAGE_CACHE_SLOTS
                ? i + 1U : PAGE_CACHE_SLOT_INVALID;
        }

        g_entries = entries;
        g_key_heads = key_heads;
        g_phys_heads = phys_heads;
        g_free_head = 0U;
        g_resident_count = 0U;
        return true;
    }

    [[nodiscard]] auto find_key_locked(void* node, u32 index) noexcept
        -> cache_entry* {
        if (!g_entries) return nullptr;
        for (u32 slot = g_key_heads[key_bucket(node, index)];
             slot != PAGE_CACHE_SLOT_INVALID;
             slot = g_entries[slot].key_next) {
            auto& entry = g_entries[slot];
            if (entry.state != cache_entry_state::empty &&
                entry.file_node == node && entry.index == index) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto find_phys_locked(u64 phys) noexcept -> cache_entry* {
        if (!g_entries) return nullptr;
        for (u32 slot = g_phys_heads[phys_bucket(phys)];
             slot != PAGE_CACHE_SLOT_INVALID;
             slot = g_entries[slot].phys_next) {
            auto& entry = g_entries[slot];
            if (entry.state != cache_entry_state::empty && entry.phys == phys) {
                return &entry;
            }
        }
        return nullptr;
    }

    auto link_key_locked(cache_entry& entry) noexcept -> void {
        const u32 bucket = key_bucket(entry.file_node, entry.index);
        entry.key_next = g_key_heads[bucket];
        g_key_heads[bucket] = index_of(entry);
    }

    auto unlink_key_locked(cache_entry& entry) noexcept -> void {
        const u32 bucket = key_bucket(entry.file_node, entry.index);
        u32* link = &g_key_heads[bucket];
        const u32 target = index_of(entry);
        while (*link != PAGE_CACHE_SLOT_INVALID) {
            if (*link == target) {
                *link = entry.key_next;
                entry.key_next = PAGE_CACHE_SLOT_INVALID;
                return;
            }
            link = &g_entries[*link].key_next;
        }
        __builtin_trap();
    }

    auto link_phys_locked(cache_entry& entry) noexcept -> void {
        const u32 bucket = phys_bucket(entry.phys);
        entry.phys_next = g_phys_heads[bucket];
        g_phys_heads[bucket] = index_of(entry);
    }

    auto unlink_phys_locked(cache_entry& entry) noexcept -> void {
        const u32 bucket = phys_bucket(entry.phys);
        u32* link = &g_phys_heads[bucket];
        const u32 target = index_of(entry);
        while (*link != PAGE_CACHE_SLOT_INVALID) {
            if (*link == target) {
                *link = entry.phys_next;
                entry.phys_next = PAGE_CACHE_SLOT_INVALID;
                return;
            }
            link = &g_entries[*link].phys_next;
        }
        __builtin_trap();
    }

    [[nodiscard]] auto reserve_loading_locked(
        void* node, u32 index
    ) noexcept -> cache_entry* {
        if (g_free_head == PAGE_CACHE_SLOT_INVALID) return nullptr;
        auto& entry = g_entries[g_free_head];
        g_free_head = entry.free_next;
        reset_entry(entry);
        entry.file_node = node;
        entry.index = index;
        entry.state = cache_entry_state::loading;
        link_key_locked(entry);
        return &entry;
    }

    auto release_entry_locked(cache_entry& entry) noexcept -> void {
        reset_entry(entry);
        entry.state = cache_entry_state::empty;
        entry.free_next = g_free_head;
        g_free_head = index_of(entry);
    }

    auto abort_loading_locked(cache_entry& entry, u32 generation) noexcept -> void {
        if (entry.generation != generation || entry.state != cache_entry_state::loading) {
            __builtin_trap();
        }
        unlink_key_locked(entry);
        release_entry_locked(entry);
    }

    auto harvest_dirty_locked(cache_entry& entry) noexcept -> void {
        if (arch::s390x::cpu::skey::skey_is_changed(
                arch::s390x::cpu::skey::iske(entry.phys))) {
            entry.dirty = true;
        }
    }

    [[nodiscard]] auto update_authority_key_locked(
        cache_entry& entry, u8 storage_key
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto policy = objects::memory_object_policy_for(entry.authority);
        if (!policy) return std::unexpected(policy.error());
        policy->storage_key = storage_key;
        ++policy->mapping_generation;
        if (policy->mapping_generation == 0U) policy->mapping_generation = 1U;
        return objects::memory_object_update_policy(entry.authority, *policy);
    }

    [[nodiscard]] auto acquire_mapping_locked(
        cache_entry& entry, u8 storage_key
    ) noexcept -> std::expected<page_lease, lib::kernel_error> {
        if (entry.state != cache_entry_state::ready) {
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::busy));
        }

        if (entry.mapping_refs == 0U) {
            harvest_dirty_locked(entry);
            auto policy_update = update_authority_key_locked(entry, storage_key);
            if (!policy_update) return std::unexpected(policy_update.error());
            entry.storage_key = storage_key;
            arch::s390x::cpu::skey::sske(entry.phys,
                arch::s390x::cpu::skey::make_skey(storage_key, false));
        } else if (entry.storage_key != storage_key) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }

        auto pin = pmm::frame_ref(entry.phys);
        if (!pin) return std::unexpected(pin.error());
        ++entry.mapping_refs;
        return page_lease{entry.phys, cache_binding{
            .slot = index_of(entry), .generation = entry.generation}};
    }

    [[nodiscard]] auto begin_evict_locked(
        cache_entry& entry
    ) noexcept -> bool {
        if (entry.state != cache_entry_state::ready || entry.mapping_refs != 0U) {
            return false;
        }
        harvest_dirty_locked(entry);
        if (entry.dirty) return false;
        entry.state = cache_entry_state::evicting;
        unlink_phys_locked(entry);
        unlink_key_locked(entry);
        return true;
    }

    auto restore_ready_locked(cache_entry& entry, u32 generation) noexcept -> void {
        if (entry.generation != generation || entry.state != cache_entry_state::evicting) {
            __builtin_trap();
        }
        entry.state = cache_entry_state::ready;
        link_key_locked(entry);
        link_phys_locked(entry);
    }

} // namespace zxfoundation::memory::page_cache::detail

namespace zxfoundation::memory::page_cache {

    using namespace detail;

    [[nodiscard]] auto release_binding(cache_binding& binding) noexcept
        -> std::expected<void, lib::kernel_error>;

    [[nodiscard]] auto duplicate_binding(
        const cache_binding& source, u8 storage_key
    ) noexcept -> std::expected<cache_binding, lib::kernel_error>;

    [[nodiscard]] auto evict_phys(u64 phys) noexcept -> bool;

    /// @brief Install the filesystem page reader used to populate cache misses.
    [[nodiscard]] auto install_reader(read_page_fn reader) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (!reader) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        const zxfoundation::sync::irq_lock_guard guard{g_lock};
        if (!ensure_entries_locked()) return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::out_of_memory));
        g_reader = reader;
        return {};
    }

    /// @brief Install the filesystem page writer used before dirty eviction.
    [[nodiscard]] auto install_writer(write_page_fn writer) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (!writer) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        const zxfoundation::sync::irq_lock_guard guard{g_lock};
        if (!ensure_entries_locked()) return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::out_of_memory));
        g_writer = writer;
        return {};
    }

    /// @brief Find or populate a page and acquire one VM mapping lease.
    [[nodiscard]] auto acquire_page(
        void* node, u32 index, u8 storage_key
    ) noexcept -> std::expected<page_lease, lib::kernel_error> {
        if (!node) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));

        cache_entry* loading = nullptr;
        u32 loading_generation = 0U;
        read_page_fn reader = nullptr;
        for (;;) {
            u64 eviction_candidate = 0ULL;
            {
                const zxfoundation::sync::irq_lock_guard guard{g_lock};
                if (!ensure_entries_locked()) return std::unexpected(
                    lib::kernel_error::from_memory(lib::memory_error::out_of_memory));
                if (auto* found = find_key_locked(node, index)) {
                    return acquire_mapping_locked(*found, storage_key);
                }
                if (!g_reader) return std::unexpected(lib::kernel_error::from_driver(
                    lib::driver_error::not_ready));
                loading = reserve_loading_locked(node, index);
                if (loading) {
                    loading_generation = loading->generation;
                    reader = g_reader;
                    break;
                }
                for (u32 i = 0U; i < PAGE_CACHE_SLOTS; ++i) {
                    const auto& entry = g_entries[i];
                    if (entry.state == cache_entry_state::ready &&
                        entry.mapping_refs == 0U) {
                        eviction_candidate = entry.phys;
                        break;
                    }
                }
            }
            if (eviction_candidate == 0ULL || !evict_phys(eviction_candidate)) {
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::out_of_memory));
            }
        }

        auto allocation = pmm::alloc(0U, pmm::zone::normal,
            pmm::purpose::file_cache, pmm::page_init::zeroed);
        if (!allocation) {
            const zxfoundation::sync::irq_lock_guard guard{g_lock};
            abort_loading_locked(*loading, loading_generation);
            return std::unexpected(allocation.error());
        }
        auto active = std::move(*allocation).make_active();
        const u64 phys = active.phys_base;
        if (reader(node, index, active.virt()) != 0U) {
            pmm::free(std::move(active));
            const zxfoundation::sync::irq_lock_guard guard{g_lock};
            abort_loading_locked(*loading, loading_generation);
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::io_error));
        }

        auto authority = objects::memory_object_create(
            scoms::kobject::kobject_type::storage,
            scoms::kobject::KOBJECT_ID_INVALID,
            nullptr,
            phys,
            PAGE_SIZE,
             dgp::isolation::isolation_rights::none,
            objects::memory_object_policy{
                .storage_key = storage_key,
                .dgp_access = dgp::memory_contract::memory_access::read,
                .mapping_generation = 1U,
            });
        if (!authority) {
            pmm::free(std::move(active));
            const zxfoundation::sync::irq_lock_guard guard{g_lock};
            abort_loading_locked(*loading, loading_generation);
            return std::unexpected(authority.error());
        }

        // The cache's resident pin is distinct from every VM mapping pin.
        auto resident_pin = pmm::frame_ref(phys);
        if (!resident_pin) {
            discard_value objects::memory_object_destroy((*authority)->ko.id);
            pmm::free(std::move(active));
            const zxfoundation::sync::irq_lock_guard guard{g_lock};
            abort_loading_locked(*loading, loading_generation);
            return std::unexpected(resident_pin.error());
        }

        auto metadata = pmm::frame_set_mapping_metadata(
            phys, (*authority)->ko.id, (*authority)->ko.generation, false);
        if (!metadata) {
            discard_value pmm::frame_release_pin(phys);
            discard_value objects::memory_object_destroy((*authority)->ko.id);
            const zxfoundation::sync::irq_lock_guard guard{g_lock};
            abort_loading_locked(*loading, loading_generation);
            return std::unexpected(metadata.error());
        }

        arch::s390x::cpu::skey::sske(phys,
            arch::s390x::cpu::skey::make_skey(storage_key, false));
        {
            const zxfoundation::sync::irq_lock_guard guard{g_lock};
            if (loading->generation != loading_generation ||
                loading->state != cache_entry_state::loading) {
                __builtin_trap();
            }
            loading->phys = phys;
            loading->storage_key = storage_key;
            loading->authority = objects::memory_object_ref_of(**authority);
            loading->state = cache_entry_state::ready;
            link_phys_locked(*loading);
            ++g_resident_count;
            return acquire_mapping_locked(*loading, storage_key);
        }
    }

    /// @brief Consume a failed-to-install lease and release its mapping pin.
    [[nodiscard]] auto release(page_lease&& lease) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (!lease) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        auto result = release_binding(lease.binding);
        if (!result) return std::unexpected(result.error());
        lease.phys = 0ULL;
        return {};
    }

    /// @brief Release the VM mapping pin represented by a durable slot binding.
    [[nodiscard]] auto release_binding(cache_binding& binding) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (!binding || binding.slot >= PAGE_CACHE_SLOTS) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }

        const zxfoundation::sync::irq_lock_guard guard{g_lock};
        if (!g_entries) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
        auto& entry = g_entries[binding.slot];
        if (entry.generation != binding.generation ||
            entry.state != cache_entry_state::ready || entry.mapping_refs == 0U) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        harvest_dirty_locked(entry);
        auto pin = pmm::frame_unref(entry.phys);
        if (!pin) return std::unexpected(pin.error());
        --entry.mapping_refs;
        binding.clear();
        return {};
    }

    /// @brief Acquire a second mapping pin for the same cache page.
    [[nodiscard]] auto duplicate_binding(
        const cache_binding& source, u8 storage_key
    ) noexcept -> std::expected<cache_binding, lib::kernel_error> {
        if (!source || source.slot >= PAGE_CACHE_SLOTS) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        const zxfoundation::sync::irq_lock_guard guard{g_lock};
        if (!g_entries) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
        auto& entry = g_entries[source.slot];
        if (entry.generation != source.generation) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        auto lease = acquire_mapping_locked(entry, storage_key);
        if (!lease) return std::unexpected(lease.error());
        return std::move(*lease).into_binding();
    }

    /// @brief Mark the page held by a live VM slot dirty.
    [[nodiscard]] auto mark_dirty_binding(
        const cache_binding& binding
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (!binding || binding.slot >= PAGE_CACHE_SLOTS) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        const zxfoundation::sync::irq_lock_guard guard{g_lock};
        if (!g_entries) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
        auto& entry = g_entries[binding.slot];
        if (entry.generation != binding.generation ||
            entry.state != cache_entry_state::ready) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        entry.dirty = true;
        return {};
    }

    /// @brief Return the number of cache-resident pages.
    [[nodiscard]] auto resident_count() noexcept -> u32 {
        const zxfoundation::sync::irq_lock_guard guard{g_lock};
        return g_resident_count;
    }

    /// @brief Reclaim up to @p target_pages unmapped cache pages.
    [[nodiscard]] auto reclaim_unmapped(u32 target_pages) noexcept -> u32 {
        constexpr u32 RECLAIM_BATCH = 64U;
        if (target_pages == 0U) return 0U;

        u32 reclaimed = 0U;
        while (reclaimed < target_pages) {
            u64 candidates[RECLAIM_BATCH]{};
            u32 candidate_count = 0U;
            {
                const zxfoundation::sync::irq_lock_guard guard{g_lock};
                if (!g_entries) break;
                const u32 remaining = target_pages - reclaimed;
                const u32 batch_limit = std::min(remaining, RECLAIM_BATCH);
                for (u32 i = 0U; i < PAGE_CACHE_SLOTS
                     && candidate_count < batch_limit; ++i) {
                    const auto& entry = g_entries[i];
                    if (entry.state != cache_entry_state::ready
                        || entry.mapping_refs != 0U) continue;
                    candidates[candidate_count++] = entry.phys;
                }
            }

            if (candidate_count == 0U) break;
            u32 progress = 0U;
            for (u32 i = 0U; i < candidate_count; ++i) {
                if (evict_phys(candidates[i])) ++progress;
            }
            reclaimed += progress;
            if (progress == 0U) break;
        }
        return reclaimed;
    }

    /// @brief Probe dirty state by physical page through the PFN hash index.
    [[nodiscard]] auto is_dirty_phys(u64 phys) noexcept -> bool {
        const zxfoundation::sync::irq_lock_guard guard{g_lock};
        auto* entry = find_phys_locked(phys);
        if (!entry || entry->state != cache_entry_state::ready) return false;
        harvest_dirty_locked(*entry);
        return entry->dirty;
    }

    /// @brief Mark a cache page dirty by physical address.
    auto mark_dirty_phys(u64 phys) noexcept -> void {
        const zxfoundation::sync::irq_lock_guard guard{g_lock};
        auto* entry = find_phys_locked(phys);
        if (entry && entry->state == cache_entry_state::ready) entry->dirty = true;
    }

    /// @brief Clear software and hardware dirty state after a successful writeback.
    auto clear_dirty_phys(u64 phys) noexcept -> void {
        const zxfoundation::sync::irq_lock_guard guard{g_lock};
        auto* entry = find_phys_locked(phys);
        if (!entry || entry->state != cache_entry_state::ready) return;
        entry->dirty = false;
        arch::s390x::cpu::skey::sske(phys,
            arch::s390x::cpu::skey::make_skey(entry->storage_key, false));
    }

    /// @brief Write back and evict a clean, unmapped cache page.
    /// @return True only when the cache's resident PMM pin was released.
    [[nodiscard]] auto evict_phys(u64 phys) noexcept -> bool {
        cache_entry* entry = nullptr;
        write_page_fn writer = nullptr;
        void* node = nullptr;
        u32 index = 0U;
        u32 generation = 0U;
        bool needs_writeback = false;

        {
            const zxfoundation::sync::irq_lock_guard guard{g_lock};
            entry = find_phys_locked(phys);
            if (!entry || entry->state != cache_entry_state::ready ||
                entry->mapping_refs != 0U) return false;
            harvest_dirty_locked(*entry);
            if (entry->dirty) {
                if (!g_writer) return false;
                entry->state = cache_entry_state::writeback;
                writer = g_writer;
                node = entry->file_node;
                index = entry->index;
                generation = entry->generation;
                needs_writeback = true;
            }
        }

        if (needs_writeback) {
            if (writer(node, index, reinterpret_cast<const void*>(
                    hhdm::phys_to_virt(phys))) != 0U) {
                const zxfoundation::sync::irq_lock_guard guard{g_lock};
                if (entry->generation == generation &&
                    entry->state == cache_entry_state::writeback) {
                    entry->state = cache_entry_state::ready;
                }
                return false;
            }
            const zxfoundation::sync::irq_lock_guard guard{g_lock};
            if (entry->generation != generation ||
                entry->state != cache_entry_state::writeback) {
                __builtin_trap();
            }
            entry->dirty = false;
            arch::s390x::cpu::skey::sske(phys,
                arch::s390x::cpu::skey::make_skey(entry->storage_key, false));
            entry->state = cache_entry_state::ready;
        }

        u32 evict_generation = 0U;
        {
            const zxfoundation::sync::irq_lock_guard guard{g_lock};
            entry = find_phys_locked(phys);
            if (!entry || !begin_evict_locked(*entry)) return false;
            evict_generation = entry->generation;
        }

        auto released = pmm::frame_release_pin(phys);
        const zxfoundation::sync::irq_lock_guard guard{g_lock};
        if (!released || !*released) {
            restore_ready_locked(*entry, evict_generation);
            return false;
        }
        if (entry->generation != evict_generation ||
            entry->state != cache_entry_state::evicting) {
            __builtin_trap();
        }
        auto destroy_authority = objects::memory_object_destroy(entry->authority);
        if (!destroy_authority) __builtin_trap();
        release_entry_locked(*entry);
        --g_resident_count;
        return true;
    }

    /// @brief Evict cache pages contained in a physical drain range.
    [[nodiscard]] auto evict_range(u64 phys_start, u64 phys_end) noexcept -> u32 {
        if (phys_end <= phys_start) return 0U;
        u64 candidates[PAGE_CACHE_SLOTS]{};
        u32 count = 0U;
        {
            const zxfoundation::sync::irq_lock_guard guard{g_lock};
            if (!g_entries) return 0U;
            for (u32 i = 0U; i < PAGE_CACHE_SLOTS; ++i) {
                const auto& entry = g_entries[i];
                if (entry.state != cache_entry_state::ready ||
                    entry.phys < phys_start || entry.phys >= phys_end) continue;
                candidates[count++] = entry.phys;
            }
        }

        u32 evicted = 0U;
        for (u32 i = 0U; i < count; ++i) {
            if (evict_phys(candidates[i])) ++evicted;
        }
        return evicted;
    }

} // namespace zxfoundation::memory::page_cache

