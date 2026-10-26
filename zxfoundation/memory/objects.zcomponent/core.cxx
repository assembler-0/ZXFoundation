/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/memory/objects.zcomponent/core.cxx
/// @brief Unified SCOMS-backed memory object hierarchy — implementation.

module zxfoundation.memory.objects.core;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.scoms.table;
import zxfoundation.scoms.metadata.core;
import zxfoundation.dgp.memory_contract;
import zxfoundation.dgp.isolation.types;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sys.zinit.core;
import zxfoundation.sync.lockable;
import zxfoundation.dgp.cap.core;
import zxfoundation.dgp.cap.types;
import lib.error;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace zxfoundation::memory::objects::detail {

    no_export scoms::scoms_table<memory_object, MAX_MEMORY_OBJECTS> g_table{};
    no_export bool g_initialized{false};

    [[nodiscard]] auto policy_from(const memory_object& object) noexcept
        -> memory_object_policy {
        return memory_object_policy{
            .storage_key = object.storage_key,
            .storage_key_generation = object.storage_key_generation,
            .numa_node = object.numa_node,
            .dgp_scope = object.dgp_scope,
            .dgp_access = object.dgp_access,
            .dgp_rights = object.dgp_rights,
            .invalidate = object.invalidate,
            .dgp_generation = object.dgp_generation,
            .mapping_generation = object.mapping_generation,
        };
    }

    auto assign_policy(memory_object& object, const memory_object_policy& policy)
        noexcept -> void {
        object.storage_key = policy.storage_key;
        object.storage_key_generation = policy.storage_key_generation;
        object.numa_node = policy.numa_node;
        object.dgp_scope = policy.dgp_scope;
        object.dgp_access = policy.dgp_access;
        object.dgp_rights = policy.dgp_rights;
        object.invalidate = policy.invalidate;
        object.dgp_generation = policy.dgp_generation != 0U
            ? policy.dgp_generation
            : zxfoundation::dgp::memory_contract::contract_generation();
        object.mapping_generation = policy.mapping_generation;
    }

    [[nodiscard]] auto valid_memory_type(scoms::kobject::kobject_type type) noexcept -> bool {
        switch (type) {
            case scoms::kobject::kobject_type::memory_resource:
            case scoms::kobject::kobject_type::memory_extent:
            case scoms::kobject::kobject_type::memory_folio:
            case scoms::kobject::kobject_type::page:
            case scoms::kobject::kobject_type::storage:
            case scoms::kobject::kobject_type::grant:
            case scoms::kobject::kobject_type::mapping:
            case scoms::kobject::kobject_type::address_space:
            case scoms::kobject::kobject_type::page_table:
            case scoms::kobject::kobject_type::key:
            case scoms::kobject::kobject_type::vm_region:
            case scoms::kobject::kobject_type::kmem_cache:
            case scoms::kobject::kobject_type::policy_server:
            case scoms::kobject::kobject_type::service_entry:
            case scoms::kobject::kobject_type::device_binding:
            case scoms::kobject::kobject_type::dataset_extent:
                return true;
            default:
                return false;
        }
    }

} // namespace zxfoundation::memory::objects::detail

namespace zxfoundation::memory::objects {

    auto zi_memory_objects_init(const sys::zinit::zi_desc&) noexcept -> sys::zinit::zi_result {
        if (detail::g_initialized) return {};
        detail::g_table.init(scoms::kobject::kobject_type::none);
        detail::g_initialized = true;
        return zxfoundation::dgp::cap::register_lookup(
            zxfoundation::dgp::cap::cap_type::memory_extent,
        [](u16 object_id) noexcept -> zxfoundation::scoms::kobject::bobject* {
            auto* object = zxfoundation::memory::objects::memory_object_resolve_id(
                static_cast<u32>(object_id));
            return object && object->ko.type == zxfoundation::scoms::kobject::kobject_type::memory_extent
                ? &object->ko : nullptr;
        });
    }
    __zinit_dep__(mm, p70, zi_memory_objects_init, fatal, "zi_vm_init")


    [[nodiscard]] auto memory_object_ref_of(
        const memory_object& object
    ) noexcept -> memory_object_ref {
        return memory_object_ref{object.ko.type, object.ko.id, object.ko.generation};
    }

    [[nodiscard]] auto memory_object_resolve(
        memory_object_ref ref
    ) noexcept -> std::expected<memory_object*, lib::kernel_error> {
        auto* object = detail::g_table.find_raw(ref.id);
        if (!object || object->ko.type != ref.type ||
            object->ko.generation != ref.generation ||
            object->ko.state == base::lifecycle_state::dying ||
            object->ko.state == base::lifecycle_state::destroyed) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));
        }
        return object;
    }

    [[nodiscard]] auto memory_object_resolve_id(
        u32 id, memory_view_kind expected_kind
    ) noexcept -> memory_object* {
        auto* object = detail::g_table.find_raw(id);
        if (!object ||
            object->ko.state == base::lifecycle_state::dying ||
            object->ko.state == base::lifecycle_state::destroyed) {
            return nullptr;
        }
        if (expected_kind != memory_view_kind::none &&
            object->view_kind != expected_kind) return nullptr;
        return object;
    }

    [[nodiscard]] auto memory_object_create(
        scoms::kobject::kobject_type type,
        u32 owner_domain,
        scoms::kobject::bobject* parent,
        u64 range_start,
        u64 range_length,
         ::zxfoundation::dgp::right_mask authority_rights,
        memory_object_policy policy
    ) noexcept -> std::expected<memory_object*, lib::kernel_error> {
        if (!detail::g_initialized || !detail::valid_memory_type(type)) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        if (type == scoms::kobject::kobject_type::memory_extent &&
            (range_start == 0U || range_length == 0U ||
             (range_start & (PAGE_SIZE - 1U)) != 0U ||
             (range_length & (PAGE_SIZE - 1U)) != 0U ||
             range_start + range_length < range_start)) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::invalid_address));
        }

        auto result = detail::g_table.alloc(type);
        if (!result) return std::unexpected(result.error());
        auto embryo_ref = static_cast<
            scoms::kobject::robject<memory_object, base::lifecycle_state::embryo>&&
        >(result.value());
        auto& object = *embryo_ref;
        object.view_kind = memory_view_kind::none;
        object.body = {};
        object.owner_domain = owner_domain;
        object.range_start = range_start;
        object.range_length = range_length;
        object.related_type = scoms::kobject::kobject_type::none;
        object.related_id = scoms::kobject::KOBJECT_ID_INVALID;
        object.related_generation = 0U;
        object.authority_rights = authority_rights;
        detail::assign_policy(object, policy);

        auto owner_result = scoms::object_core::set_owner(object.ko, owner_domain);
        if (!owner_result) {
            discard_value detail::g_table.release(object.ko.id);
            return std::unexpected(owner_result.error());
        }
        if (parent) {
            auto attach_result = scoms::object_core::attach(object.ko, *parent);
            if (!attach_result) {
                discard_value scoms::object_core::detach(object.ko);
                discard_value detail::g_table.release(object.ko.id);
                return std::unexpected(attach_result.error());
            }
            object.related_type = parent->type;
            object.related_id = parent->id;
            object.related_generation = parent->generation;
        }

        auto active_ref = static_cast<
            scoms::kobject::robject<memory_object, base::lifecycle_state::embryo>&&
        >(embryo_ref).activate();
        if (!active_ref) {
            discard_value scoms::object_core::detach(object.ko);
            discard_value detail::g_table.release(object.ko.id);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        return &object;
    }

    [[nodiscard]] auto memory_object_attach_view(
        memory_object_ref ref,
        memory_view_kind kind, void* body
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (kind == memory_view_kind::none || !body) return std::unexpected(
            lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        auto r = memory_object_resolve(ref);
        if (!r) return std::unexpected(r.error());
        auto& obj = **r;
        const sync::irq_lock_guard _{obj.ko.lock};
        if (obj.view_kind != memory_view_kind::none) return std::unexpected(
            lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        obj.view_kind = kind;
        switch (kind) {
            case memory_view_kind::vm_region:      obj.body.vm_region = body;      break;
            case memory_view_kind::kmem_cache:       obj.body.kmem_cache = body;     break;
            case memory_view_kind::policy_server:    obj.body.policy_server = body;  break;
            case memory_view_kind::service_entry:    obj.body.service_entry = body;  break;
            default: break;
        }
        return {};
    }

    [[nodiscard]] auto memory_object_detach_view(
        memory_object_ref ref
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto r = memory_object_resolve(ref);
        if (!r) return std::unexpected(r.error());
        auto& obj = **r;
        const sync::irq_lock_guard _{obj.ko.lock};
        obj.view_kind = memory_view_kind::none;
        obj.body = {};
        return {};
    }

    [[nodiscard]] auto memory_object_link(
        memory_object& object, const memory_object& related
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (object.ko.state != base::lifecycle_state::active ||
            related.ko.state != base::lifecycle_state::active) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        const sync::irq_lock_guard _{object.ko.lock};
        object.related_type = related.ko.type;
        object.related_id = related.ko.id;
        object.related_generation = related.ko.generation;
        return {};
    }

    [[nodiscard]] auto memory_object_destroy(u32 id) noexcept
        -> std::expected<void, lib::kernel_error> {
        auto* object = detail::g_table.find_raw(id);
        if (!object) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
        bool extent_was_retiring = false;
        if (object->ko.type == scoms::kobject::kobject_type::memory_extent) {
            const sync::irq_lock_guard _{object->ko.lock};
            if (object->active_mappings.load(std::memory_order::acquire) != 0U) {
                return std::unexpected(lib::kernel_error::from_driver(
                    lib::driver_error::busy));
            }
            extent_was_retiring = object->extent_retiring.exchange(
                true, std::memory_order::acq_rel);
        }
        if (scoms::object_core::has_dependents(object->ko)) {
            if (object->ko.type == scoms::kobject::kobject_type::memory_extent &&
                !extent_was_retiring) {
                object->extent_retiring.store(false, std::memory_order::release);
            }
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::busy));
        }
        auto result = detail::g_table.release(id);
        if (!result && object->ko.type == scoms::kobject::kobject_type::memory_extent &&
            !extent_was_retiring) {
            object->extent_retiring.store(false, std::memory_order::release);
        }
        return result;
    }

    [[nodiscard]] auto memory_object_delegate(
        memory_object_ref source, u32 caller_domain,
        u32 owner_domain,  ::zxfoundation::dgp::right_mask authority_rights
    ) noexcept -> std::expected<memory_object_ref, lib::kernel_error> {
        auto source_result = memory_object_resolve(source);
        if (!source_result) return std::unexpected(source_result.error());
        auto& original = **source_result;
        if (original.ko.type != scoms::kobject::kobject_type::grant ||
            caller_domain != original.owner_domain ||
            owner_domain == caller_domain ||
            authority_rights ==  ::zxfoundation::dgp::right::none ||
            !dgp::has_all_rights(original.authority_rights, authority_rights) ||
            !dgp::has_all_rights(original.authority_rights,
                 ::zxfoundation::dgp::right::delegate)) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        auto policy = memory_object_policy_for(source);
        if (!policy) return std::unexpected(policy.error());
        auto delegated = memory_object_create(
            scoms::kobject::kobject_type::grant, owner_domain, &original.ko,
            original.range_start, original.range_length, authority_rights, *policy);
        if (!delegated) return std::unexpected(delegated.error());
        return memory_object_ref_of(**delegated);
    }

    [[nodiscard]] auto memory_object_policy_for(
        memory_object_ref ref
    ) noexcept -> std::expected<memory_object_policy, lib::kernel_error> {
        auto object_result = memory_object_resolve(ref);
        if (!object_result) return std::unexpected(object_result.error());
        auto& object = **object_result;
        const sync::irq_lock_guard _{object.ko.lock};
        return detail::policy_from(object);
    }

    [[nodiscard]] auto memory_object_update_policy(
        memory_object_ref storage, const memory_object_policy& policy
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto object_result = memory_object_resolve(storage);
        if (!object_result) return std::unexpected(object_result.error());
        auto& object = **object_result;
        if (object.ko.type != scoms::kobject::kobject_type::storage &&
            object.ko.type != scoms::kobject::kobject_type::mapping) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        const sync::irq_lock_guard _{object.ko.lock};
        detail::assign_policy(object, policy);
        return {};
    }

    [[nodiscard]] auto memory_object_destroy(
        memory_object_ref ref
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto object_result = memory_object_resolve(ref);
        if (!object_result) return std::unexpected(object_result.error());
        return memory_object_destroy((*object_result)->ko.id);
    }

    [[nodiscard]] auto memory_object_update_range(
        memory_object_ref ref, u64 range_start, u64 range_length
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto object_result = memory_object_resolve(ref);
        if (!object_result) return std::unexpected(object_result.error());
        auto& object = **object_result;
        object.range_start = range_start;
        object.range_length = range_length;
        return {};
    }

    [[nodiscard]] auto memory_extent_acquire_mapping(
        memory_object_ref extent
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto object_result = memory_object_resolve(extent);
        if (!object_result) return std::unexpected(object_result.error());
        auto& object = **object_result;
        if (object.ko.type != scoms::kobject::kobject_type::memory_extent) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        const sync::irq_lock_guard _{object.ko.lock};
        if (object.ko.state != base::lifecycle_state::active ||
            object.extent_retiring.load(std::memory_order::acquire)) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));
        }
        object.active_mappings.fetch_add(1U, std::memory_order::acq_rel);
        return {};
    }

    [[nodiscard]] auto memory_extent_release_mapping(
        memory_object_ref extent
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto object_result = memory_object_resolve(extent);
        if (!object_result) return std::unexpected(object_result.error());
        auto& object = **object_result;
        if (object.ko.type != scoms::kobject::kobject_type::memory_extent) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        u32 current = object.active_mappings.load(std::memory_order::acquire);
        while (current != 0U) {
            if (object.active_mappings.compare_exchange_weak(
                    current, current - 1U, std::memory_order::acq_rel,
                    std::memory_order::acquire)) return {};
        }
        return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::purpose_mismatch));
    }

    [[nodiscard]] auto memory_extent_mapping_count(
        memory_object_ref extent
    ) noexcept -> std::expected<u32, lib::kernel_error> {
        auto object_result = memory_object_resolve(extent);
        if (!object_result) return std::unexpected(object_result.error());
        auto& object = **object_result;
        if (object.ko.type != scoms::kobject::kobject_type::memory_extent) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        return object.active_mappings.load(std::memory_order::acquire);
    }

    [[nodiscard]] auto memory_extent_retire(
        memory_object_ref extent
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto object_result = memory_object_resolve(extent);
        if (!object_result) return std::unexpected(object_result.error());
        auto& object = **object_result;
        if (object.ko.type != scoms::kobject::kobject_type::memory_extent) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        const sync::irq_lock_guard _{object.ko.lock};
        if (object.active_mappings.load(std::memory_order::acquire) != 0U ||
            object.extent_retiring.exchange(true, std::memory_order::acq_rel)) {
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::busy));
        }
        return {};
    }

    [[nodiscard]] auto memory_object_find(u32 id) noexcept -> memory_object* {
        return detail::g_table.find_raw(id);
    }

} // namespace zxfoundation::memory::objects

