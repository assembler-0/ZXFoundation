/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/gate.zcomponent/registration.cxxm
/// @brief Register all handlers into the dispatch table.

module zxfoundation.dgp.gate.registration;
import zxfoundation.dgp.gate.types;
import zxfoundation.dgp.gate.dispatch;
import zxfoundation.dgp.gate.nc_dom;
import zxfoundation.dgp.gate.nc_sgn;
import zxfoundation.dgp.gate.nc_cap;
import zxfoundation.dgp.gate.nc_gate;
import zxfoundation.dgp.gate.nc_io;
import zxfoundation.dgp.gate.nc_mem;
import zxfoundation.dgp.gate.nc_portal;
import zxfoundation.dgp.gate.nc_sys;
import zxfoundation.dgp.authorize;
import zxfoundation.sys.printk.core;
import zxfoundation.base.types;
import lib.error;
import std;

namespace zxfoundation::dgp::gate::registration {

    using zxfoundation::dgp::gate::dispatch::dispatch_handler;
    using zxfoundation::dgp::gate::dispatch::entry_meta;
    using zxfoundation::dgp::gate::dispatch::gate_auth_level;
    using zxfoundation::dgp::gate::dispatch::registration_op;
    using zxfoundation::dgp::gate::dispatch::register_namespace_handlers;
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
                make_op(0, nc_dom::dispatch_wrap<dom_op::create>,              gate_auth_level::admin,       operation_kind::domain_create),
                make_op(1, nc_dom::dispatch_wrap<dom_op::destroy>,            gate_auth_level::admin,       operation_kind::domain_destroy),
                make_op(2, nc_dom::dispatch_wrap<dom_op::exit>,               gate_auth_level::none,        operation_kind::domain_exit),
                make_op(3, nc_dom::dispatch_wrap<dom_op::info>,               gate_auth_level::none,        operation_kind::domain_info),
                make_op(4, nc_dom::dispatch_wrap<dom_op::query>,              gate_auth_level::none,        operation_kind::domain_query),
                make_op(5, nc_dom::dispatch_wrap<dom_op::strand_create>,      gate_auth_level::admin,       operation_kind::strand_create),
                make_op(6, nc_dom::dispatch_wrap<dom_op::strand_set_policy>,   gate_auth_level::admin,       operation_kind::strand_set_policy),
                make_op(7, nc_dom::dispatch_wrap<dom_op::strand_set_affinity>, gate_auth_level::admin,       operation_kind::strand_set_affinity),
                make_op(8, nc_dom::dispatch_wrap<dom_op::strand_query_state>,  gate_auth_level::admin,       operation_kind::strand_query_state),
            }, 9);

        register_ns("mem",    call_namespace::mem,
            (const registration_op[]){
                make_op(0, nc_mem::dispatch_wrap<mem_op::alloc>,    gate_auth_level::none,        operation_kind::mem_alloc),
                make_op(1, nc_mem::dispatch_wrap<mem_op::free>,     gate_auth_level::none,        operation_kind::mem_free),
                make_op(2, nc_mem::dispatch_wrap<mem_op::map>,      gate_auth_level::none,        operation_kind::mem_map),
                make_op(3, nc_mem::dispatch_wrap<mem_op::unmap>,    gate_auth_level::none,        operation_kind::mem_unmap),
                make_op(4, nc_mem::dispatch_wrap<mem_op::protect>,  gate_auth_level::none,        operation_kind::mem_protect),
                make_op(5, nc_mem::dispatch_wrap<mem_op::share>,    gate_auth_level::none,        operation_kind::mem_share),
            }, 6);

        register_ns("gate",   call_namespace::gate,
            (const registration_op[]){
                make_op(0, nc_gate::dispatch_wrap<gate_op::create>,  gate_auth_level::admin,      operation_kind::gate_create),
                make_op(1, nc_gate::dispatch_wrap<gate_op::destroy>, gate_auth_level::admin,      operation_kind::gate_destroy),
            }, 2);

        register_ns("portal", call_namespace::portal,
            (const registration_op[]){
                make_op(0, nc_portal::dispatch_wrap<portal_op::create>,  gate_auth_level::admin,   operation_kind::portal_create),
                make_op(1, nc_portal::dispatch_wrap<portal_op::destroy>, gate_auth_level::admin,     operation_kind::portal_destroy),
            }, 2);

        register_ns("io",     call_namespace::io,
            (const registration_op[]){
                make_op(0, nc_io::dispatch_wrap<io_op::bind>,     gate_auth_level::io,  operation_kind::io_bind),
                make_op(1, nc_io::dispatch_wrap<io_op::unbind>,   gate_auth_level::io,  operation_kind::io_unbind),
                make_op(2, nc_io::dispatch_wrap<io_op::submit>,   gate_auth_level::io,  operation_kind::io_submit),
                make_op(3, nc_io::dispatch_wrap<io_op::irq_bind>, gate_auth_level::io,  operation_kind::io_irq_bind),
                make_op(4, nc_io::dispatch_wrap<io_op::dma_extent>, gate_auth_level::io, operation_kind::io_dma_extent),
                make_op(5, nc_io::dispatch_wrap<io_op::completion_dequeue>, gate_auth_level::io, operation_kind::io_completion_dequeue),
            }, 6);

        register_ns("sgn",    call_namespace::sgn,
            (const registration_op[]){
                make_op(0, nc_sgn::dispatch_wrap<sgn_op::register_handler>, gate_auth_level::none,    operation_kind::signal_register),
                make_op(1, nc_sgn::dispatch_wrap<sgn_op::poll>,             gate_auth_level::none,    operation_kind::signal_poll),
                make_op(2, nc_sgn::dispatch_wrap<sgn_op::ack>,              gate_auth_level::none,    operation_kind::signal_ack),
                make_op(3, nc_sgn::dispatch_wrap<sgn_op::send>,             gate_auth_level::signal,  operation_kind::signal_send),
                make_op(4, nc_sgn::dispatch_wrap<sgn_op::mask_set>,         gate_auth_level::none,    operation_kind::signal_mask_set),
                make_op(5, nc_sgn::dispatch_wrap<sgn_op::mask_get>,         gate_auth_level::none,    operation_kind::signal_mask_get),
                make_op(6, nc_sgn::dispatch_wrap<sgn_op::wait>,             gate_auth_level::none,    operation_kind::signal_wait),
            }, 7);

        register_ns("cap",    call_namespace::cap,
            (const registration_op[]){
                make_op(0, nc_cap::dispatch_wrap<cap_op::acquire>,  gate_auth_level::none,    operation_kind::cap_acquire),
                make_op(1, nc_cap::dispatch_wrap<cap_op::release>,  gate_auth_level::none,    operation_kind::cap_release),
                make_op(2, nc_cap::dispatch_wrap<cap_op::validate>, gate_auth_level::none,    operation_kind::cap_validate),
                make_op(3, nc_cap::dispatch_wrap<cap_op::revoke>,   gate_auth_level::admin,   operation_kind::cap_revoke),
            }, 4);

        register_ns("sys",    call_namespace::sys,
            (const registration_op[]){
                make_op(0, nc_sys::dispatch_wrap<sys_op::time_yield>,         gate_auth_level::none,        operation_kind::time_yield),
                make_op(1, nc_sys::dispatch_wrap<sys_op::time_deadline>,      gate_auth_level::none,        operation_kind::time_deadline),
                make_op(2, nc_sys::dispatch_wrap<sys_op::time_now>,           gate_auth_level::none,        operation_kind::time_now),
                make_op(3, nc_sys::dispatch_wrap<sys_op::console_write>,      gate_auth_level::none,        operation_kind::console_write),
                make_op(4, nc_sys::dispatch_wrap<sys_op::register_service>,   gate_auth_level::capability,  operation_kind::register_service),
                make_op(5, nc_sys::dispatch_wrap<sys_op::lookup_service>,     gate_auth_level::none,        operation_kind::lookup_service),
                make_op(6, nc_sys::dispatch_wrap<sys_op::unregister_service>, gate_auth_level::none,        operation_kind::unregister_service),
                make_op(7, nc_sys::dispatch_wrap<sys_op::ipc_endpoint_create>, gate_auth_level::none, operation_kind::ipc_endpoint_create),
                make_op(8, nc_sys::dispatch_wrap<sys_op::ipc_endpoint_destroy>, gate_auth_level::none, operation_kind::ipc_endpoint_destroy),
                make_op(9, nc_sys::dispatch_wrap<sys_op::ipc_send>, gate_auth_level::none, operation_kind::ipc_send),
                make_op(10, nc_sys::dispatch_wrap<sys_op::ipc_receive>, gate_auth_level::none, operation_kind::ipc_receive),
                make_op(11, nc_sys::dispatch_wrap<sys_op::shm_export>, gate_auth_level::none, operation_kind::shm_export),
                make_op(12, nc_sys::dispatch_wrap<sys_op::shm_import>, gate_auth_level::none, operation_kind::shm_import),
                make_op(13, nc_sys::dispatch_wrap<sys_op::shm_map>, gate_auth_level::none, operation_kind::shm_map),
                make_op(14, nc_sys::dispatch_wrap<sys_op::policy_register>, gate_auth_level::none, operation_kind::policy_server_register),
                make_op(15, nc_sys::dispatch_wrap<sys_op::policy_unregister>, gate_auth_level::none, operation_kind::policy_server_unregister),
              }, 16);
    }

} // namespace zxfoundation::dgp::gate::registration
