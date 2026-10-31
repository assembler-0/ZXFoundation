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
import zxfoundation.sys.printk.core;
import zxfoundation.base.config;
import arch.s390x.cpu.percpu;
import lib.stl;
import zxfoundation.dgp.authorize;
import zxfoundation.dgp.isolation.types;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.sys.zinit.core;
import lib.error;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace zxfoundation::dgp::policy::detail {

    static scoms::scoms_table<policy_server, MAX_POLICY_SERVERS> g_table{};
    alignas(4) static sync::qspinlock::qspinlock<sync::lock_rank::policy_table> g_lock{};
    static bool g_initialized{false};
    // Consult transport plumbing: never reused for anything else.
    static std::atomic<u64> g_consult_serial{1ULL};
    static inline std::array<std::atomic<bool>,
        zxfoundation::base::config::CONFIG_ZX_MAX_CPUS> g_consult_active{};

    [[nodiscard]] static auto valid_kind(server_kind kind) noexcept -> bool {
        return static_cast<u8>(kind) < static_cast<u8>(server_kind::count);
    }

} // namespace zxfoundation::dgp::policy::detail

namespace zxfoundation::dgp::policy {

    /// @brief Consult transport (ch. §8.3): one decision request to the
    ///        registered server's endpoint, bounded wait for the reply.
    ///        Fail-safe envelope — any transport failure or timeout is the
    ///        kernel default ({allow, 0}); userland answers can only deny
    ///        (never silently allow more than the rights check permitted).
    static auto consult_transport(u32 caller_domain_id, server_kind kind,
                                  u64 request_a0, u64 request_a1,
                                  u64 timeout_ns) noexcept -> decision {
        if (!detail::g_initialized) return {};   // pre-registry: default
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        // Re-entrancy guard: a consult must never consult (an ipc op inside
        // the consult's own transport would loop through this path).
        if (detail::g_consult_active[pcp.cpu_id].load(std::memory_order::relaxed))
            return {};
        if (detail::g_consult_active[pcp.cpu_id].exchange(true,
                std::memory_order::acquire))
            return {};

        decision out{};
        {
            u32 cid = caller_domain_id;
            if (cid == 0U) {
                auto* strand = reinterpret_cast<
                    zxfoundation::dgp::domain::strand*>(
                    static_cast<u64>(pcp.current_strand));
                cid = (strand && strand->owner)
                    ? reinterpret_cast<zxfoundation::dgp::domain::domain*>(
                        strand->owner)->ko.id : 0U;
            }
            auto* caller = cid ? zxfoundation::dgp::domain::domain_find(cid)
                               : nullptr;
            if (caller) {
                u32 endpoint_id = 0U;
                bool have_server = false;
                {
                    const sync::irq_lock_guard _{detail::g_lock};
                    auto* server = find_server(kind);
                    have_server = server && server->ko.state ==
                                  base::lifecycle_state::active;
                    if (have_server) endpoint_id = server->endpoint_id;
                }
                if (have_server) {
                    u32 payload[4]{static_cast<u32>(kind),
                                   static_cast<u32>(request_a0),
                                   static_cast<u32>(request_a1),
                                   static_cast<u32>(caller_domain_id)};
                    const u64 correlation =
                        detail::g_consult_serial.fetch_add(1U,
                            std::memory_order::relaxed) | 1ULL;
                    auto sent = zxfoundation::sys::ipc::message_send(
                        *caller, endpoint_id,
                        reinterpret_cast<const u8*>(payload), sizeof(payload),
                        nullptr, 0U, nullptr, 0U, correlation);
                    if (sent) {
                        u64 status = 0ULL;
                        auto waited = zxfoundation::sys::ipc::message_call_wait(
                            *caller, endpoint_id, correlation, timeout_ns,
                            status);
                        if (waited && status != 0ULL) {
                            out.outcome = decide_outcome::deny;
                            out.status = static_cast<u32>(status & 0xFFFFFFFFULL);
                        }
                    }
                    // send/wait failure or timeout = default decision.
                }
            }
        }
        detail::g_consult_active[pcp.cpu_id].store(false,
            std::memory_order::release);
        return out;
    }

    auto zi_policy_init(const sys::zinit::zi_desc&) noexcept -> sys::zinit::zi_result {
        if (detail::g_initialized) return {};
        detail::g_table.init(scoms::kobject::kobject_type::policy_server);
        detail::g_consult_serial = 1ULL;
        g_decide = &consult_transport;
        detail::g_initialized = true;
        return {};
    }
    __zinit__(core, p10, zi_policy_init, fatal)

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
                .requested_rights =  ::zxfoundation::dgp::right::naming_service});
        if (!auth) return std::unexpected(auth.error());

        const sync::irq_lock_guard _{detail::g_lock};
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
             ::zxfoundation::dgp::right::naming_service);
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
        const sync::irq_lock_guard _{detail::g_lock};
        auto auth = dgp::authorize(
            owner, owner, dgp::operation_kind::policy_server_unregister,
            dgp::authorization_metadata{
                .requested_rights =  ::zxfoundation::dgp::right::naming_service});
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
        const sync::irq_lock_guard _{detail::g_lock};
        detail::g_table.destroy_if(
            [domain_id](const policy_server& server) noexcept -> bool {
                auto* mo =
                    memory::objects::memory_object_resolve_id(server.authority.id);
                return mo && mo->owner_domain == domain_id;
            },
            [](policy_server& server) noexcept -> void {
                // Same hygiene as unregister_server: the authority object
                // and the owner attach must not survive the reaper.
                discard_value memory::objects::memory_object_destroy(
                    server.authority);
                server.authority = {};
                discard_value scoms::object_core::detach(server.ko);
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

