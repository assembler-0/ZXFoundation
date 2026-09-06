/// SPDX-License-Identifier: Apache-2.0
/// @file scoms_object_core.cxxm
/// @brief Nucleus-owned metadata for the unified object ontology.

module zxfoundation.scoms.metadata.core;
import zxfoundation.scoms.kobject.types;
import zxfoundation.scoms.kobject.base;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.base.types;
import lib.error;
import lib.stl;

#define __zxmacro_want_symalias
#include macro_extensions

auto allocate(u64) noexcept -> void* symalias_reference(kmalloc);

namespace zxfoundation::scoms::object_core::detail {

    no_export std::array<object_metadata*, METADATA_BLOCK_COUNT> g_metadata{};
    alignas(4) no_export sync::qspinlock::qspinlock g_lock{};

    [[nodiscard]] no_export auto metadata_at(u32 index, bool create = false) noexcept
        -> object_metadata* {
        const u32 block = index / METADATA_BLOCK_SIZE;
        if (!g_metadata[block]) {
            if (!create) return nullptr;
            g_metadata[block] = static_cast<object_metadata*>(
                allocate(sizeof(object_metadata) * METADATA_BLOCK_SIZE));
            if (!g_metadata[block]) return nullptr;
            for (u32 i = 0; i < METADATA_BLOCK_SIZE; ++i)
                g_metadata[block][i] = {};
        }
        return &g_metadata[block][index % METADATA_BLOCK_SIZE];
    }

    [[nodiscard]] no_export auto find_locked(const kobject::bobject& object) noexcept -> object_metadata* {
        if (object.object_flags == 0U) return nullptr;
        const u32 index = object.object_flags - 1U;
        if (index >= MAX_OBJECT_METADATA) return nullptr;
        auto* entry = metadata_at(index);
        return entry && entry->active && entry->object == &object ? entry : nullptr;
    }

    [[nodiscard]] no_export auto retired_locked(const kobject::bobject& object) noexcept -> object_metadata* {
        if (object.object_flags == 0U) return nullptr;
        const u32 index = object.object_flags - 1U;
        if (index >= MAX_OBJECT_METADATA) return nullptr;
        auto* entry = metadata_at(index);
        return entry && entry->reclaim_pending && entry->object == &object ? entry : nullptr;
    }

    [[nodiscard]] no_export auto error_not_found() noexcept -> lib::kernel_error {
        return lib::kernel_error::from_generic(lib::generic_error::not_found);
    }

    no_export auto clear_parent_locked(object_metadata& child) noexcept -> void {
        if (child.parent_id == kobject::KOBJECT_ID_INVALID) return;

        if (child.parent_slot != 0U) {
            auto* parent = metadata_at(child.parent_slot - 1U);
            if (parent && parent->active && parent->object &&
                parent->object->type == child.parent_type &&
                parent->object->id == child.parent_id &&
                parent->object->generation == child.parent_generation &&
                parent->dependent_count != 0U) {
                --parent->dependent_count;
            }
        }

        child.parent_slot = 0;
        child.parent_type = kobject::kobject_type::none;
        child.parent_id = kobject::KOBJECT_ID_INVALID;
        child.parent_generation = 0;
    }

} // namespace zxfoundation::scoms::object_core::detail

namespace zxfoundation::scoms::object_core {

    [[nodiscard]] auto register_object(
        kobject::bobject& object, u32 owner_domain
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const sync::irq_lock_guard guard{detail::g_lock};
        if (detail::find_locked(object)) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        for (u32 i = 0; i < MAX_OBJECT_METADATA; ++i) {
            auto* entry = detail::metadata_at(i, true);
            if (!entry || entry->active || entry->reclaim_pending) continue;
            entry->object = &object;
            entry->owner_domain = owner_domain;
            entry->parent_slot = 0;
            entry->parent_type = kobject::kobject_type::none;
            entry->parent_id = kobject::KOBJECT_ID_INVALID;
            entry->parent_generation = 0;
            entry->object_flags = object.object_flags;
            entry->dependent_count = 0;
            entry->hardware_generation = 0;
            entry->reclaimer = nullptr;
            entry->active = true;
            entry->reclaim_pending = false;
            object.object_flags = i + 1U;
            return {};
        }
        return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::out_of_memory));
    }

    [[nodiscard]] auto unregister_object(
        kobject::bobject& object
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const sync::irq_lock_guard guard{detail::g_lock};
        auto* entry = detail::find_locked(object);
        if (!entry) return std::unexpected(detail::error_not_found());
        if (entry->dependent_count != 0U) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        detail::clear_parent_locked(*entry);
        entry->active = false;
        entry->reclaim_pending = true;
        return {};
    }

    [[nodiscard]] auto set_reclaimer(
        kobject::bobject& object, void (*reclaimer)(kobject::bobject&) noexcept
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (!reclaimer) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        const sync::irq_lock_guard guard{detail::g_lock};
        auto* entry = detail::find_locked(object);
        if (!entry) return std::unexpected(detail::error_not_found());
        entry->reclaimer = reclaimer;
        return {};
    }

    auto finalize_last_put(kobject::bobject& object) noexcept -> void {
        void (*reclaimer)(kobject::bobject&) noexcept = nullptr;
        {
            const sync::irq_lock_guard guard{detail::g_lock};
            auto* entry = detail::retired_locked(object);
            if (!entry || !entry->reclaimer) __builtin_trap();
            reclaimer = entry->reclaimer;
        }
        reclaimer(object);
    }

    auto complete_reclaim(kobject::bobject& object) noexcept -> void {
        const sync::irq_lock_guard guard{detail::g_lock};
        auto* entry = detail::retired_locked(object);
        if (!entry) __builtin_trap();
        entry->object = nullptr;
        entry->reclaimer = nullptr;
        entry->reclaim_pending = false;
        object.object_flags = 0U;
    }

    [[nodiscard]] auto set_owner(
        kobject::bobject& object, u32 owner_domain
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const sync::irq_lock_guard guard{detail::g_lock};
        auto* entry = detail::find_locked(object);
        if (!entry) return std::unexpected(detail::error_not_found());
        entry->owner_domain = owner_domain;
        return {};
    }

    [[nodiscard]] auto attach(
        kobject::bobject& child, kobject::bobject& parent
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (&child == &parent) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        const sync::irq_lock_guard guard{detail::g_lock};
        auto* child_meta = detail::find_locked(child);
        auto* parent_meta = detail::find_locked(parent);
        if (!child_meta || !parent_meta || child_meta->parent_id != kobject::KOBJECT_ID_INVALID) {
            return std::unexpected(detail::error_not_found());
        }
        child_meta->parent_type = parent.type;
        child_meta->parent_id = parent.id;
        child_meta->parent_generation = parent.generation;
        child_meta->parent_slot = parent.object_flags;
        ++parent_meta->dependent_count;
        return {};
    }

    [[nodiscard]] auto detach(
        kobject::bobject& child
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const sync::irq_lock_guard guard{detail::g_lock};
        auto* child_meta = detail::find_locked(child);
        if (!child_meta) return std::unexpected(detail::error_not_found());
        if (child_meta->parent_id == kobject::KOBJECT_ID_INVALID) return {};

        detail::clear_parent_locked(*child_meta);
        return {};
    }

    [[nodiscard]] auto has_dependents(
        const kobject::bobject& object
    ) noexcept -> bool {
        const sync::irq_lock_guard guard{detail::g_lock};
        const auto* entry = detail::find_locked(object);
        return entry != nullptr && entry->dependent_count != 0U;
    }

    [[nodiscard]] auto bump_hardware_generation(
        kobject::bobject& object
    ) noexcept -> std::expected<u32, lib::kernel_error> {
        const sync::irq_lock_guard guard{detail::g_lock};
        auto* entry = detail::find_locked(object);
        if (!entry) return std::unexpected(detail::error_not_found());
        ++entry->hardware_generation;
        return entry->hardware_generation;
    }

    [[nodiscard]] auto metadata(
        const kobject::bobject& object
    ) noexcept -> std::expected<object_metadata, lib::kernel_error> {
        const sync::irq_lock_guard guard{detail::g_lock};
        const auto* entry = detail::find_locked(object);
        if (!entry) return std::unexpected(detail::error_not_found());
        return *entry;
    }

} // namespace zxfoundation::scoms::object_core
