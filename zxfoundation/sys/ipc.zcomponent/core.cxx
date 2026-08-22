/// SPDX-License-Identifier: Apache-2.0
/// @file ipc.cxxm
/// @brief Bounded endpoint/message object management and grant transfer.

module zxfoundation.sys.ipc.core;
import zxfoundation.sys.ipc.types;
import zxfoundation.base.types;
import zxfoundation.scoms.table;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.scoms.metadata.core;
import zxfoundation.memory.objects;
import zxfoundation.memory.vm;
import zxfoundation.memory.vm.types;
import zxfoundation.memory.kmalloc;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.cap.types;
import zxfoundation.dgp.cap.core;
import zxfoundation.dgp.domain.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.portal;
import zxfoundation.dgp.portal.types;
import zxfoundation.dgp.authorize;
import zxfoundation.dgp.isolation.types;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.scoms.kobject.base;
import zxfoundation.sync.lockable;
import zxfoundation.base.typestate;
import zxfoundation.sys.printk.core;
import arch.s390x.cpu.lowcore;
import lib.error;
import std;

namespace zxfoundation::sys::ipc::detail {

    static scoms::scoms_table<endpoint_object, MAX_ENDPOINTS> g_endpoints{};
    static scoms::scoms_table<message_object, MAX_MESSAGES> g_messages{};
    static bool g_initialized{false};
    alignas(4) static sync::qspinlock::qspinlock g_lock{};

    [[nodiscard]] auto endpoint(u32 id) noexcept -> endpoint_object* {
        return g_endpoints.find_raw(id);
    }

    [[nodiscard]] auto message(u32 id) noexcept -> message_object* {
        return g_messages.find_raw(id);
    }

    auto release_message(message_object& msg) noexcept -> void {
        for (u32 i = 0; i < msg.capability_count; ++i) {
            discard_value dgp::cap::cap_release(msg.body->capabilities[i].token);
        }
        for (u32 i = 0; i < msg.grant_count; ++i) {
            discard_value memory::objects::memory_object_destroy(
                msg.body->grants[i].grant);
        }
        if (msg.body) memory::kmalloc::kfree_t(msg.body);
        msg.body = nullptr;
        discard_value g_messages.release(msg.ko.id);
    }

} // namespace zxfoundation::sys::ipc::detail

namespace zxfoundation::sys::ipc {

    auto ipc_init() noexcept -> void {
        if (detail::g_initialized) return;
        detail::g_endpoints.init(scoms::kobject::kobject_type::endpoint);
        detail::g_messages.init(scoms::kobject::kobject_type::message);
        detail::g_initialized = true;
    }

    [[nodiscard]] auto endpoint_create(
        dgp::domain::domain& owner, u32 peer_domain
    ) noexcept -> std::expected<u32, lib::kernel_error> {
        auto* peer = dgp::domain::domain_find(peer_domain);
        if (!peer) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
        auto auth = dgp::authorize(
            owner, *peer, dgp::operation_kind::ipc_endpoint_create,
            dgp::authorization_metadata{
                .requested_rights =  dgp::isolation::isolation_rights::signal});
        if (!auth) return std::unexpected(auth.error());

        const sync::irq_lock_guard guard{detail::g_lock};
        auto result = detail::g_endpoints.alloc();
        if (!result) return std::unexpected(result.error());
        auto ref = static_cast<scoms::kobject::robject<endpoint_object,
            base::lifecycle_state::embryo>&&>(result.value());
        auto& endpoint = *ref;
        endpoint.owner_domain = owner.ko.id;
        endpoint.peer_domain = peer_domain;
        endpoint.queue = memory::kmalloc::kmalloc_t<endpoint_queue>();
        if (!endpoint.queue) {
            discard_value detail::g_endpoints.release(endpoint.ko.id);
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::out_of_memory));
        }
        auto owner_result = scoms::object_core::set_owner(endpoint.ko, owner.ko.id);
        auto attach_result = scoms::object_core::attach(endpoint.ko, owner.ko);
        if (!owner_result || !attach_result) {
            memory::kmalloc::kfree_t(endpoint.queue);
            endpoint.queue = nullptr;
            discard_value detail::g_endpoints.release(endpoint.ko.id);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        const u32 id = endpoint.ko.id;
        discard_value static_cast<scoms::kobject::robject<endpoint_object,
            base::lifecycle_state::embryo>&&>(ref).activate();
        if (!auth->consume()) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        return id;
    }

    [[nodiscard]] auto endpoint_destroy(
        dgp::domain::domain& caller, u32 endpoint_id
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const sync::irq_lock_guard guard{detail::g_lock};
        auto* endpoint = detail::endpoint(endpoint_id);
        if (!endpoint || endpoint->owner_domain != caller.ko.id) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        auto* peer = dgp::domain::domain_find(endpoint->peer_domain);
        if (!peer) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
        auto auth = dgp::authorize(
            caller, *peer, dgp::operation_kind::ipc_endpoint_destroy,
            dgp::authorization_metadata{
                .requested_rights =  dgp::isolation::isolation_rights::signal});
        if (!auth) return std::unexpected(auth.error());
        while (endpoint->queue_count != 0U) {
            auto* msg = detail::message(endpoint->queue->message_ids[endpoint->queue_head]);
            endpoint->queue_head = (endpoint->queue_head + 1U) % IPC_QUEUE_DEPTH;
            --endpoint->queue_count;
            if (msg) detail::release_message(*msg);
        }
        memory::kmalloc::kfree_t(endpoint->queue);
        endpoint->queue = nullptr;
        if (!auth->consume()) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        return detail::g_endpoints.release(endpoint_id);
    }

    [[nodiscard]] auto endpoint_owned_by(
        u32 endpoint_id, u32 domain_id
    ) noexcept -> bool {
        auto* endpoint = detail::endpoint(endpoint_id);
        return endpoint && endpoint->owner_domain == domain_id;
    }

    auto destroy_for_domain(u32 domain_id) noexcept -> void {
        detail::g_endpoints.destroy_if(
            [domain_id](const endpoint_object& endpoint) noexcept -> bool {
                return endpoint.owner_domain == domain_id ||
                       endpoint.peer_domain == domain_id;
            },
            [](endpoint_object& endpoint) noexcept -> void {
                while (endpoint.queue && endpoint.queue_count != 0U) {
                    auto* msg = detail::message(
                        endpoint.queue->message_ids[endpoint.queue_head]);
                    endpoint.queue_head = (endpoint.queue_head + 1U) % IPC_QUEUE_DEPTH;
                    --endpoint.queue_count;
                    if (msg) detail::release_message(*msg);
                }
                if (endpoint.queue) memory::kmalloc::kfree_t(endpoint.queue);
                endpoint.queue = nullptr;
            });
    }

    [[nodiscard]] auto message_send(
        dgp::domain::domain& caller, u32 endpoint_id,
        const u8* payload, u32 length,
        const dgp::cap::cap_token* capabilities, u32 capability_count,
        const memory::objects::memory_object_ref* grants, u32 grant_count
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (!payload || length > IPC_MESSAGE_BYTES ||
            capability_count > IPC_MESSAGE_CAPS || grant_count > IPC_MESSAGE_GRANTS) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        const sync::irq_lock_guard guard{detail::g_lock};
        auto* endpoint = detail::endpoint(endpoint_id);
        if (!endpoint || endpoint->owner_domain != caller.ko.id) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        auto* peer = dgp::domain::domain_find(endpoint->peer_domain);
        if (!peer || endpoint->queue_count >= IPC_QUEUE_DEPTH) {
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::busy));
        }
        auto auth = dgp::authorize(
            caller, *peer, dgp::operation_kind::ipc_send,
            dgp::authorization_metadata{
                .requested_rights =  dgp::isolation::isolation_rights::signal});
        if (!auth) return std::unexpected(auth.error());
        auto result = detail::g_messages.alloc();
        if (!result) return std::unexpected(result.error());
        auto ref = static_cast<scoms::kobject::robject<message_object,
            base::lifecycle_state::embryo>&&>(result.value());
        auto& msg = *ref;
        msg.body = memory::kmalloc::kmalloc_t<message_body>();
        if (!msg.body) {
            discard_value detail::g_messages.release(msg.ko.id);
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::out_of_memory));
        }
        msg.sender_domain = caller.ko.id;
        msg.length = length;
        msg.capability_count = capability_count;
        msg.grant_count = grant_count;
        std::copy_n(payload, length, msg.body->payload);
        for (u32 i = 0; i < capability_count; ++i) {
            auto duplicate = dgp::cap::cap_duplicate_for(
                capabilities[i], capabilities[i].rights(), caller.ko.id,
                peer->ko.id);
            if (!duplicate) {
                detail::release_message(msg);
                return std::unexpected(duplicate.error());
            }
            msg.body->capabilities[i].token = *duplicate;
        }
        for (u32 i = 0; i < grant_count; ++i) {
            auto grant_result = memory::objects::memory_object_resolve(grants[i]);
            if (!grant_result || (*grant_result)->ko.type != scoms::kobject::kobject_type::grant) {
                detail::release_message(msg);
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::capability_invalid));
            }
            if ((*grant_result)->owner_domain != caller.ko.id) {
                detail::release_message(msg);
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::capability_invalid));
            }
            const auto attenuation = (*grant_result)->authority_rights
                & ( dgp::isolation::isolation_rights::map |  dgp::isolation::isolation_rights::share);
            auto delegated = memory::objects::memory_object_delegate(
                grants[i], caller.ko.id, peer->ko.id, attenuation);
            if (!delegated) {
                detail::release_message(msg);
                return std::unexpected(delegated.error());
            }
            msg.body->grants[i].grant = *delegated;
            msg.body->grants[i].rights = static_cast<u32>(attenuation);
        }
        discard_value static_cast<scoms::kobject::robject<message_object,
            base::lifecycle_state::embryo>&&>(ref).activate();
        endpoint->queue->message_ids[endpoint->queue_tail] = msg.ko.id;
        endpoint->queue_tail = (endpoint->queue_tail + 1U) % IPC_QUEUE_DEPTH;
        ++endpoint->queue_count;
        if (!auth->consume()) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        return {};
    }

    [[nodiscard]] auto message_receive(
        dgp::domain::domain& caller, u32 endpoint_id,
        u8* payload, u32 payload_capacity, u32& out_length,
        dgp::cap::cap_token* capabilities, u32& capability_count,
        memory::objects::memory_object_ref* grants, u32& grant_count
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (!payload || !capabilities || !grants) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        const sync::irq_lock_guard guard{detail::g_lock};
        auto* endpoint = detail::endpoint(endpoint_id);
        if (!endpoint || endpoint->peer_domain != caller.ko.id ||
            endpoint->queue_count == 0U) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));
        }
        auto* msg = detail::message(endpoint->queue->message_ids[endpoint->queue_head]);
        if (!msg || payload_capacity < msg->length) return std::unexpected(
            lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        std::copy_n(msg->body->payload, msg->length, payload);
        out_length = msg->length;
        if (capability_count < msg->capability_count || grant_count < msg->grant_count) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        for (u32 i = 0; i < msg->capability_count; ++i)
            capabilities[i] = msg->body->capabilities[i].token;
        for (u32 i = 0; i < msg->grant_count; ++i)
            grants[i] = msg->body->grants[i].grant;
        capability_count = msg->capability_count;
        grant_count = msg->grant_count;
        msg->capability_count = 0;
        msg->grant_count = 0;
        endpoint->queue_head = (endpoint->queue_head + 1U) % IPC_QUEUE_DEPTH;
        --endpoint->queue_count;
        detail::release_message(*msg);
        return {};
    }

[[nodiscard]] auto shm_export(
    dgp::domain::domain& caller, u64 va, u64 length, u32 target_domain
) noexcept -> std::expected<dgp::cap::cap_token, lib::kernel_error> {
    if (length == 0 || (va & (PAGE_SIZE - 1ULL)) != 0 ||
        (length & (PAGE_SIZE - 1ULL)) != 0 || va + length < va) {
        sys::printk::perr("shm_export: bad va/length va={:#x} len={}", va, length);
        return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::invalid_address));
    }
    auto* target = dgp::domain::domain_find(target_domain);
    if (!target) {
        sys::printk::perr("shm_export: target domain {} not found", target_domain);
        return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
    }
    auto auth = dgp::authorize(
        caller, *target, dgp::operation_kind::shm_export,
        dgp::authorization_metadata{
            .requested_rights =  dgp::isolation::isolation_rights::share,
            .target_va_start = va, .target_va_end = va + length});
    if (!auth) {
        sys::printk::perr("shm_export: DGP authorize denied");
        return std::unexpected(auth.error());
    }
    auto* region = memory::vm::vm_find(caller.vm_space, va);
    if (!region) {
        sys::printk::perr("shm_export: vm_find failed va={:#x}", va);
        return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::capability_invalid));
    }
    if (va != region->va_start || length != region->size() ||
        region->owner_domain != caller.ko.id || !region->grant_object ||
        !region->mapping_object) {
        sys::printk::perr("shm_export: region mismatch va={:#x}/{:#x} len={:#x}/{:#x} owner={} (caller={}) grant={}",
            va, region->va_start, length, region->size(),
            region->owner_domain, caller.ko.id,
            static_cast<bool>(region->grant_object));
        return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::capability_invalid));
    }
    auto grant = memory::objects::memory_object_resolve(region->grant_object);
    auto mapping = memory::objects::memory_object_resolve(region->mapping_object);
    if (!grant || !mapping || (*grant)->ko.type != scoms::kobject::kobject_type::grant ||
        (*grant)->owner_domain != caller.ko.id ||
        (*grant)->range_start != va || (*grant)->range_length != length ||
        (*mapping)->ko.type != scoms::kobject::kobject_type::mapping ||
        (*mapping)->related_type != scoms::kobject::kobject_type::grant ||
        (*mapping)->related_id != (*grant)->ko.id ||
        (*mapping)->related_generation != (*grant)->ko.generation) {
        return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::capability_invalid));
    }
    auto token = dgp::cap::cap_issue(
        dgp::cap::cap_type::memory, static_cast<u16>((*grant)->ko.id),
        dgp::cap::cap_rights::read | dgp::cap::cap_rights::map |
            dgp::cap::cap_rights::delegate,
        target_domain);
        if (!token) {
            sys::printk::perr("shm_export: cap_issue failed err={} grant.id={} grant.state={}",
                token.error(), (*grant)->ko.id,
                static_cast<u32>((*grant)->ko.state));
            return std::unexpected(token.error());
        }
    if (!auth->consume()) {
        sys::printk::perr("shm_export: auth consume failed");
        return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
    }
    return *token;
}

    [[nodiscard]] auto shm_import(
        dgp::domain::domain& caller, dgp::cap::cap_token grant_token,
        u64 va, u64 length
    ) noexcept -> std::expected<dgp::portal::portal_binding, lib::kernel_error> {
        if (length == 0 || (va & (PAGE_SIZE - 1ULL)) != 0 ||
            (length & (PAGE_SIZE - 1ULL)) != 0 || va + length < va)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::invalid_address));
        auto grant = dgp::cap::cap_validate<memory::objects::memory_object>(
            grant_token, dgp::cap::cap_rights::map);
        if (!grant) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        auto owner_check = dgp::cap::cap_validate_generic_for(
            grant_token, dgp::cap::cap_rights::map, caller.ko.id);
        if (!owner_check) return std::unexpected(owner_check.error());
        auto* source = dgp::domain::domain_find((*grant)->owner_domain);
        if (!source) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_found));
        if ((*grant)->ko.type != scoms::kobject::kobject_type::grant ||
            length != (*grant)->range_length)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        auto* source_region = memory::vm::vm_find_grant_mapping(
            source->vm_space, memory::objects::memory_object_ref_of(**grant));
        if (!source_region || source_region->owner_domain != source->ko.id ||
            source_region->size() != length) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        auto auth = dgp::authorize(
            *source, caller, dgp::operation_kind::shm_import,
            dgp::authorization_metadata{
                .requested_rights =  dgp::isolation::isolation_rights::map,
                .target_va_start = va, .target_va_end = va + length});
        if (!auth) return std::unexpected(auth.error());
        if (va < memory::vm::VM_USER_START || va + length < va ||
            va + length > memory::vm::VM_USER_END)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::invalid_address));
        auto portal_result = dgp::portal::portal_create(
            caller.ko.id, source->ko.id, va, va + length,
            dgp::portal::portal_prot::read, 0U, source_region);
        if (!portal_result) return std::unexpected(portal_result.error());

        auto* desc = dgp::portal::portal_find(*portal_result);
        if (!desc) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::internal_error));
        }

        auto* active = static_cast<dgp::domain::strand*>(
            arch::s390x::cpu::lowcore::current_strand_ptr());
        if (!active || active->owner != &caller || !active->body) {
            active = &caller.primary_strand;
        }
        if (!active->body) {
            discard_value dgp::portal::portal_destroy(*portal_result);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        active->body->uregs.access_regs[desc->ar_num] = desc->alet;
        if (!auth->consume()) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));
        return dgp::portal::portal_binding {
            .ar_num = desc->ar_num,
            .portal_id = *portal_result,
        };
    }

} // namespace zxfoundation::sys::ipc
