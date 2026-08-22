/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/policy.zcomponent/core.cxx
/// @brief Bounded policy-server registration owned by the nucleus.

module zxfoundation.dgp.policy.core;
import zxfoundation.dgp.policy.types;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.table;
import zxfoundation.scoms.metadata.core;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.kobject.types;
import zxfoundation.memory.objects;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.domain.core;
import zxfoundation.sys.ipc.core;
import zxfoundation.dgp.authorize;
import zxfoundation.dgp.isolation.types;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import lib.error;
import std;

namespace zxfoundation::dgp::policy::detail {

    no_export scoms::scoms_table<policy_server, MAX_POLICY_SERVERS> g_table{};
    alignas(4) no_export sync::qspinlock::qspinlock g_lock{};
    no_export bool g_initialized{false};

    [[nodiscard]] no_export auto valid_kind(server_kind kind) noexcept -> bool {
        return static_cast<u8>(kind) < static_cast<u8>(server_kind::count);
    }

} // namespace zxfoundation::dgp::policy::detail

namespace zxfoundation::dgp::policy {

    auto init() noexcept -> void {
        if (detail::g_initialized) return;
        detail::g_table.init(scoms::kobject::kobject_type::policy_server);
        detail::g_initialized = true;
    }

    [[nodiscard]] auto register_server(
        dgp::domain::domain& owner, server_kind kind, u32 endpoint_id
    ) noexcept -> std::expected<u32, lib::kernel_error> {
        if (!detail::g_initialized || owner.ko.state != base::lifecycle_state::active ||
            !detail::valid_kind(kind) ||
            !sys::ipc::endpoint_owned_by(endpoint_id, owner.ko.id))
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));

        auto auth = dgp::authorize(
            owner, owner, dgp::operation_kind::policy_server_register,
            dgp::authorization_metadata{
                .requested_rights =  dgp::isolation::isolation_rights::service});
        if (!auth) return std::unexpected(auth.error());

        const sync::irq_lock_guard guard{detail::g_lock};
        for (u32 i = 0; i < MAX_POLICY_SERVERS; ++i) {
            auto* existing = detail::g_table.find_raw(i);
            if (existing && existing->kind == kind)
                return std::unexpected(lib::kernel_error::from_generic(
                    lib::generic_error::internal_error));
        }

        auto result = detail::g_table.alloc(scoms::kobject::kobject_type::policy_server);
        if (!result) return std::unexpected(result.error());
        auto ref = static_cast<scoms::kobject::robject<policy_server,
            base::lifecycle_state::embryo>&&>(result.value());
        auto& server = *ref;
        server.kind = kind;
        server.endpoint_id = endpoint_id;

        auto mo_result = memory::objects::memory_object_create(
            scoms::kobject::kobject_type::policy_server, owner.ko.id,
            &owner.ko, 0, 0,
             dgp::isolation::isolation_rights::service);
        if (!mo_result) {
            discard_value detail::g_table.release(server.ko.id);
            return std::unexpected(mo_result.error());
        }
        server.authority = memory::objects::memory_object_ref_of(**mo_result);
        discard_value memory::objects::memory_object_attach_view(
            server.authority, memory::objects::memory_view_kind::policy_server, &server);

        auto owner_result = scoms::object_core::set_owner(server.ko, owner.ko.id);
        auto attach_result = scoms::object_core::attach(server.ko, owner.ko);
        if (!owner_result || !attach_result) {
            discard_value memory::objects::memory_object_destroy(server.authority);
            discard_value detail::g_table.release(server.ko.id);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        const u32 id = server.ko.id;
        discard_value static_cast<scoms::kobject::robject<policy_server,
            base::lifecycle_state::embryo>&&>(ref).activate();
        if (!auth->consume()) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        return id;
    }

    [[nodiscard]] auto unregister_server(
        dgp::domain::domain& owner, server_kind kind
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const sync::irq_lock_guard guard{detail::g_lock};
        auto auth = dgp::authorize(
            owner, owner, dgp::operation_kind::policy_server_unregister,
            dgp::authorization_metadata{
                .requested_rights =  dgp::isolation::isolation_rights::service});
        if (!auth) return std::unexpected(auth.error());
        for (u32 i = 0; i < MAX_POLICY_SERVERS; ++i) {
            auto* server = detail::g_table.find_raw(i);
            if (!server || server->kind != kind) continue;
            auto* mo = memory::objects::memory_object_resolve_id(server->authority.id);
            if (!mo || mo->owner_domain != owner.ko.id)
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::capability_invalid));
            discard_value scoms::object_core::detach(server->ko);
            discard_value memory::objects::memory_object_destroy(server->authority);
            server->authority = {};
            auto result = detail::g_table.release(i);
            if (!result) return result;
            if (!auth->consume()) return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
            return {};
        }
        return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
    }

    auto destroy_for_domain(u32 domain_id) noexcept -> void {
        const sync::irq_lock_guard guard{detail::g_lock};
        detail::g_table.destroy_if(
            [domain_id](const policy_server& server) noexcept -> bool {
                auto* mo = memory::objects::memory_object_resolve_id(server.authority.id);
                return mo && mo->owner_domain == domain_id;
            });
    }

    [[nodiscard]] auto find_server(server_kind kind) noexcept -> policy_server* {
        if (!detail::valid_kind(kind)) return nullptr;
        for (u32 i = 0; i < MAX_POLICY_SERVERS; ++i) {
            auto* server = detail::g_table.find_raw(i);
            if (server && server->kind == kind) return server;
        }
        return nullptr;
    }

} // namespace zxfoundation::dgp::policy

