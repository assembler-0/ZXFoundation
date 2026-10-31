/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/gate.zcomponent/registration.cxx
/// @brief Register all handlers into the dispatch table.

module zxfoundation.dgp.gate.registration;
import zxfoundation.dgp.gate.types;
import zxfoundation.dgp.svc;
import zxfoundation.dgp.authorize;
import zxfoundation.sys.printk.core;
import zxfoundation.base.types;
import lib.error;
import lib.stl;

import :svc_dom;
import :svc_sgn;
import :svc_cap;
import :svc_gate;
import :svc_io;
import :svc_mem;
import :svc_portal;
import :svc_sys;

namespace zxfoundation::dgp::gate::registration {

    using zxfoundation::dgp::svc::dispatch_handler;
    using zxfoundation::dgp::svc::entry_meta;
    using zxfoundation::dgp::svc::registration_op;
    using zxfoundation::dgp::svc::register_namespace_handlers;
    using zxfoundation::dgp::gate::call_namespace;
    using zxfoundation::dgp::gate::dom_op;
    using zxfoundation::dgp::gate::mem_op;
    using zxfoundation::dgp::gate::gate_op;
    using zxfoundation::dgp::gate::portal_op;
    using zxfoundation::dgp::gate::io_op;
    using zxfoundation::dgp::gate::sgn_op;
    using zxfoundation::dgp::gate::cap_op;
    using zxfoundation::dgp::gate::sys_op;
    using zxfoundation::dgp::operation_kind;
    using zxfoundation::sys::printk::perr;
    using zxfoundation::sys::printk::pinfo;

    /// @brief Register accessible handlers into the dispatch table.
    auto register_all_handlers() noexcept -> void {

        auto register_ns = [&](const char* name, call_namespace ns, const registration_op* ops, u32 count) {
            auto r = register_namespace_handlers(ns, ops, count);
            if (!r) {
                perr("gate: {} (ns={}) registration failed ({})", name, static_cast<u32>(ns), r.error());
            }
        };

        register_ns("dom",    call_namespace::dom,
            (const registration_op[]){
                make_op(0, svc_dom::dispatch_wrap<dom_op::create>, operation_kind::domain_create, true),
                make_op(1, svc_dom::dispatch_wrap<dom_op::destroy>, operation_kind::domain_destroy, true),
                make_op(2, svc_dom::dispatch_wrap<dom_op::exit>, operation_kind::domain_exit, false),
                make_op(3, svc_dom::dispatch_wrap<dom_op::info>, operation_kind::domain_info, false),
                make_op(4, svc_dom::dispatch_wrap<dom_op::query>, operation_kind::domain_query, false),
                make_op(5, svc_dom::dispatch_wrap<dom_op::strand_create>, operation_kind::strand_create, true),
                make_op(11, svc_dom::dispatch_wrap<dom_op::exec>, operation_kind::domain_exec, true),
                make_op(6, svc_dom::dispatch_wrap<dom_op::strand_set_policy>, operation_kind::strand_set_policy, true),
                make_op(7, svc_dom::dispatch_wrap<dom_op::strand_set_affinity>, operation_kind::strand_set_affinity, true),
                make_op(8, svc_dom::dispatch_wrap<dom_op::strand_query_state>, operation_kind::strand_query_state, true),
                make_op(9, svc_dom::dispatch_wrap<dom_op::strand_join>, operation_kind::strand_join, false),
                make_op(10, svc_dom::dispatch_wrap<dom_op::strand_exit>, operation_kind::strand_exit, false),
            }, 12);

        register_ns("mem",    call_namespace::mem,
            (const registration_op[]){
                make_op(0, svc_mem::dispatch_wrap<mem_op::alloc>, operation_kind::mem_alloc, false),
                make_op(1, svc_mem::dispatch_wrap<mem_op::free>, operation_kind::mem_free, false),
                make_op(2, svc_mem::dispatch_wrap<mem_op::map>, operation_kind::mem_map, true),
                make_op(3, svc_mem::dispatch_wrap<mem_op::unmap>, operation_kind::mem_unmap, false),
                make_op(4, svc_mem::dispatch_wrap<mem_op::protect>, operation_kind::mem_protect, false),
                make_op(6, svc_mem::dispatch_wrap<mem_op::futex_wait>, operation_kind::none, false),
                make_op(7, svc_mem::dispatch_wrap<mem_op::futex_wake>, operation_kind::none, false),
            }, 7);

        register_ns("gate",   call_namespace::gate,
            (const registration_op[]){
                make_op(0, svc_gate::dispatch_wrap<gate_op::create>, operation_kind::gate_create, true),
                make_op(1, svc_gate::dispatch_wrap<gate_op::destroy>, operation_kind::gate_destroy, true),
                make_op(2, svc_gate::dispatch_wrap<gate_op::query>, operation_kind::gate_query, true),
                make_op(3, svc_gate::dispatch_wrap<gate_op::defer>, operation_kind::none, false),
                make_op(4, svc_gate::dispatch_wrap<gate_op::resume>, operation_kind::none, false),
            }, 5);

        register_ns("portal", call_namespace::portal,
            (const registration_op[]){
                make_op(0, svc_portal::dispatch_wrap<portal_op::create>, operation_kind::portal_create, true),
                make_op(1, svc_portal::dispatch_wrap<portal_op::destroy>, operation_kind::portal_destroy, true),
            }, 2);

        register_ns("io",     call_namespace::io,
            (const registration_op[]){
                make_op(0, svc_io::dispatch_wrap<io_op::bind>, operation_kind::io_bind, false),
                make_op(1, svc_io::dispatch_wrap<io_op::unbind>, operation_kind::io_unbind, true),
                make_op(2, svc_io::dispatch_wrap<io_op::submit>, operation_kind::io_submit, true),
                make_op(4, svc_io::dispatch_wrap<io_op::dma_extent>, operation_kind::io_dma_extent, true),
                make_op(5, svc_io::dispatch_wrap<io_op::completion_dequeue>, operation_kind::io_completion_dequeue, true),
            }, 5);

        register_ns("sgn",    call_namespace::sgn,
            (const registration_op[]){
                make_op(0, svc_sgn::dispatch_wrap<sgn_op::register_handler>, operation_kind::signal_register, false),
                make_op(1, svc_sgn::dispatch_wrap<sgn_op::poll>, operation_kind::signal_poll, false),
                make_op(2, svc_sgn::dispatch_wrap<sgn_op::ack>, operation_kind::signal_ack, false),
                make_op(3, svc_sgn::dispatch_wrap<sgn_op::send>, operation_kind::signal_send, false),
                make_op(4, svc_sgn::dispatch_wrap<sgn_op::mask_set>, operation_kind::signal_mask_set, false),
                make_op(5, svc_sgn::dispatch_wrap<sgn_op::mask_get>, operation_kind::signal_mask_get, false),
                make_op(6, svc_sgn::dispatch_wrap<sgn_op::wait>, operation_kind::signal_wait, false),
            }, 7);

        register_ns("cap",    call_namespace::cap,
            (const registration_op[]){
                make_op(0, svc_cap::dispatch_wrap<cap_op::acquire>, operation_kind::cap_acquire, false),
                make_op(1, svc_cap::dispatch_wrap<cap_op::release>, operation_kind::cap_release, false),
                make_op(2, svc_cap::dispatch_wrap<cap_op::validate>, operation_kind::cap_validate, false),
                make_op(3, svc_cap::dispatch_wrap<cap_op::revoke>, operation_kind::cap_revoke, true),
            }, 4);

        register_ns("sys",    call_namespace::sys,
            (const registration_op[]){
                make_op(0, svc_sys::dispatch_wrap<sys_op::time_yield>, operation_kind::time_yield, false),
                make_op(1, svc_sys::dispatch_wrap<sys_op::time_deadline>, operation_kind::time_deadline, false),
                make_op(2, svc_sys::dispatch_wrap<sys_op::time_now>, operation_kind::time_now, false),
                make_op(20, svc_sys::dispatch_wrap<sys_op::time_sleep>, operation_kind::time_sleep, false),
                make_op(3, svc_sys::dispatch_wrap<sys_op::console_write>, operation_kind::console_write, false),
                make_op(4, svc_sys::dispatch_wrap<sys_op::register_service>, operation_kind::register_service, true),
                make_op(5, svc_sys::dispatch_wrap<sys_op::lookup_service>, operation_kind::lookup_service, false),
                make_op(6, svc_sys::dispatch_wrap<sys_op::unregister_service>, operation_kind::unregister_service, false),
                make_op(7, svc_sys::dispatch_wrap<sys_op::ipc_endpoint_create>, operation_kind::ipc_endpoint_create, false),
                make_op(8, svc_sys::dispatch_wrap<sys_op::ipc_endpoint_destroy>, operation_kind::ipc_endpoint_destroy, false),
                make_op(9, svc_sys::dispatch_wrap<sys_op::ipc_send>, operation_kind::ipc_send, false),
                make_op(10, svc_sys::dispatch_wrap<sys_op::ipc_receive>, operation_kind::ipc_receive, false),
                make_op(17, svc_sys::dispatch_wrap<sys_op::ipc_reply>, operation_kind::ipc_send, false),
                make_op(18, svc_sys::dispatch_wrap<sys_op::ipc_call>, operation_kind::ipc_send, false),
                make_op(19, svc_sys::dispatch_wrap<sys_op::signal_return>, operation_kind::none, false),
                make_op(11, svc_sys::dispatch_wrap<sys_op::shm_export>, operation_kind::shm_export, false),
                make_op(12, svc_sys::dispatch_wrap<sys_op::shm_import>, operation_kind::shm_import, false),
                make_op(13, svc_sys::dispatch_wrap<sys_op::shm_map>, operation_kind::shm_map, false),
                make_op(14, svc_sys::dispatch_wrap<sys_op::policy_register>, operation_kind::policy_server_register, false),
                make_op(15, svc_sys::dispatch_wrap<sys_op::policy_unregister>, operation_kind::policy_server_unregister, false),
                make_op(16, svc_sys::dispatch_wrap<sys_op::last_error>, operation_kind::none, false),
                make_op(21, svc_sys::dispatch_wrap<sys_op::policy_consult>, operation_kind::none, false),
              }, 22);
    }

} // namespace zxfoundation::dgp::gate::registration
