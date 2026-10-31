/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/service.zcomponent/core.cxx
/// @brief Service registry — lightweight name-to-capability directory for
///        the DGP kernel's service discovery model.

module zxfoundation.sys.service.core;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.table;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.scoms.metadata.core;
import zxfoundation.memory.objects;
import zxfoundation.dgp.cap.types;
import zxfoundation.dgp.cap.core;
import zxfoundation.dgp.domain.core;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.sys.zinit.core;
import zxfoundation.sys.printk.core;
import lib.error;
import lib.cstring;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace zxfoundation::sys::service::detail {

    static scoms::scoms_table<service_entry, MAX_SERVICES> g_service_table{};
    static bool g_service_initialized{false};

} // namespace zxfoundation::sys::service::detail

namespace zxfoundation::sys::service {

    using namespace dgp::cap;

    auto zi_service_init(const zinit::zi_desc&) noexcept -> zinit::zi_result {
        using namespace detail;
        if (g_service_initialized) return {};
        g_service_table.init(scoms::kobject::kobject_type::service_entry);
        register_service_cap_lookup();
        g_service_initialized = true;
        return {};
    }
    __zinit__(dgp, p30, zi_service_init, fatal)

    [[nodiscard]] auto register_service(
        std::string_view name,
        cap_token        token,
        u64              port,
        u32              owner_id
    ) noexcept -> std::expected<u32, lib::kernel_error> {
        using namespace detail;

        if (name.empty() || !token) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        if (name.size() >= SERVICE_NAME_MAX_LEN) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        auto token_result = cap_validate_generic_for(token, ::zxfoundation::dgp::right::none, owner_id);
        if (!token_result) return std::unexpected(token_result.error());

        for (u32 i = 0; i < MAX_SERVICES; ++i) {
            auto* existing = g_service_table.find_raw(i);
            if (existing && std::string_view(existing->name) == name) {
                return std::unexpected(
                    lib::kernel_error::from_generic(lib::generic_error::name_in_use));
            }
        }

        auto result = g_service_table.alloc();
        if (!result) {
            return std::unexpected(result.error());
        }

        auto embryo_ref = static_cast<
            scoms::kobject::robject<service_entry, base::lifecycle_state::embryo>&&
        >(result.value());

        service_entry& entry = *embryo_ref;
        auto owner_result = scoms::object_core::set_owner(entry.ko, owner_id);
        if (!owner_result) {
            discard_value g_service_table.release(entry.ko.id);
            return std::unexpected(owner_result.error());
        }
        lib::string_copy(entry.name, name);
        entry.port = port;
        auto stored_token = cap_duplicate(token, token.rights(), owner_id);
        if (!stored_token) {
            discard_value g_service_table.release(entry.ko.id);
            return std::unexpected(stored_token.error());
        }
        entry.token       = *stored_token;

        auto mo_result = memory::objects::memory_object_create(
            scoms::kobject::kobject_type::service_entry, owner_id,
            &entry.ko, 0, 0);
        if (!mo_result) {
            discard_value cap_release(*stored_token);
            discard_value g_service_table.release(entry.ko.id);
            return std::unexpected(mo_result.error());
        }
        entry.authority = memory::objects::memory_object_ref_of(**mo_result);
        discard_value memory_object_attach_view(
            entry.authority, memory::objects::memory_view_kind::service_entry, &entry);

        const u32 id = entry.ko.id;

        discard_value static_cast<
            scoms::kobject::robject<service_entry, base::lifecycle_state::embryo>&&
        >(embryo_ref).activate();

        return id;
    }

    [[nodiscard]] auto lookup_service(
        std::string_view name,
        u32              caller_id
    ) noexcept -> std::expected<u64, lib::kernel_error> {
        using namespace detail;

        if (name.empty()) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        for (u32 i = 0; i < MAX_SERVICES; ++i) {
            auto* entry = g_service_table.find_raw(i);
            if (entry && std::string_view(entry->name) == name) {
                return entry->port;
            }
        }
        return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
    }

    [[nodiscard]] auto unregister_service(
        u32 service_id,
        u32 caller_id
    ) noexcept -> std::expected<void, lib::kernel_error> {
        using namespace detail;

        auto* entry = g_service_table.find_raw(service_id);
        if (!entry) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));
        }

        auto* mo = memory::objects::memory_object_resolve_id(entry->authority.id);
        if (!mo) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));
        }
        if (mo->owner_domain != caller_id && caller_id != 0) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::denied));
        }

        char name_copy[SERVICE_NAME_MAX_LEN]{};
        lib::string_copy(name_copy, std::string_view{entry->name});
        discard_value cap_release(entry->token);
        discard_value memory::objects::memory_object_destroy(entry->authority);
        entry->authority = {};
        const auto result = g_service_table.release(service_id);
        if (!result) {
            return std::unexpected(result.error());
        }

        return {};
    }

    auto destroy_for_domain(u32 domain_id) noexcept -> void {
        detail::g_service_table.destroy_if(
            [domain_id](const service_entry& entry) noexcept -> bool {
                auto* mo = memory::objects::memory_object_resolve_id(entry.authority.id);
                return mo && mo->owner_domain == domain_id;
            },
            [](service_entry& entry) noexcept -> void {
                discard_value cap_release(entry.token);
                discard_value memory_object_destroy(entry.authority);
                entry.authority = {};
            });
    }

    auto service_count() noexcept -> u32 {
        using namespace detail;
        return g_service_table.count();
    }

    /// @brief Look up a service_entry by SCOMS ID.
    /// @return Raw pointer, or nullptr if not found.
    [[nodiscard]] auto service_entry_find_by_id(u32 id) noexcept -> service_entry* {
        return detail::g_service_table.find_raw(id);
    }

    auto register_service_cap_lookup() noexcept -> void {
        discard_value register_lookup(
            cap_type::service,
            [](u16 obj_id) noexcept -> scoms::kobject::bobject* {
                auto* e = service_entry_find_by_id(static_cast<u32>(obj_id));
                return e ? &e->ko : nullptr;
            });
    }

} // namespace zxfoundation::sys::service

namespace zxfoundation::dgp::cap {

    using zxfoundation::base::lifecycle_state;

    template <>
    [[nodiscard]] auto cap_validate<zxfoundation::sys::service::service_entry>(
        cap_token token, ::zxfoundation::dgp::right_mask needed
    ) noexcept -> std::expected<zxfoundation::sys::service::service_entry*, lib::kernel_error> {

        if (!token) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        if (token.type() != cap_type::service) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        if ((token.rights() & needed) == ::zxfoundation::dgp::right::none && needed != ::zxfoundation::dgp::right::none) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        auto result = zxfoundation::dgp::cap::cap_resolve(token, needed);
        if (!result) return std::unexpected(result.error());
        if (token.type() != cap_type::service) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        return reinterpret_cast<zxfoundation::sys::service::service_entry*>(*result);
    }

} // namespace zxfoundation::dgp::cap
