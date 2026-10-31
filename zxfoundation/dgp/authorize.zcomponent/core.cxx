/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/authorize.zcomponent/core.cxx
/// @brief Unified DGP authorization.

module zxfoundation.dgp.authorize.core;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.sync_contract.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.cap.types;
import zxfoundation.dgp.cap.core;
import zxfoundation.dgp.policy.types;
import zxfoundation.sys.printk.core;
import zxfoundation.sched.context;
import zxfoundation.scoms.kobject.base;
import lib.error;
import lib.stl;

namespace zxfoundation::dgp::detail {

    /// @brief Which policy kind does an operation's consult belong to
    [[nodiscard]] static auto policy_kind_for_op(operation_kind op) noexcept
        -> ::std::optional<zxfoundation::dgp::policy::server_kind> {
        using op_k = operation_kind;
        using sk = zxfoundation::dgp::policy::server_kind;
        switch (op) {
        case op_k::register_service: return {sk::services};
        case op_k::domain_exec:      return {sk::loader};
        case op_k::io_bind:          return {sk::drivers};
        default:                     return ::std::nullopt;
        }
    }

    [[nodiscard]] auto validate_common(
        const zxfoundation::dgp::domain::domain& caller,
        const zxfoundation::dgp::domain::domain& target
    ) noexcept -> std::expected<void, lib::kernel_error>
    {
        if (caller.ko.state != zxfoundation::base::lifecycle_state::active) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        if (target.ko.state != zxfoundation::base::lifecycle_state::active) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        auto sync_state = sync_contract::contract_snapshot(
            sync_contract::lock_context::thread,
            sync_contract::teardown_phase::none,
            caller.ko.id);
        auto sync_result = validate_authority_lookup(sync_state);
        if (!sync_result) {
            return std::unexpected(sync_result.error());
        }

        return {};
    }

    [[nodiscard]] constexpr auto required_rights_for_op(operation_kind op) noexcept
        -> ::zxfoundation::dgp::right_mask
    {
        switch (op) {
            case operation_kind::domain_create:    return ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy );
            case operation_kind::domain_destroy:   return ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy );
            case operation_kind::gate_create:      return ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy );
            case operation_kind::gate_destroy:     return ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy );
            case operation_kind::portal_destroy:   return ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy );
            case operation_kind::domain_exec:      return ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy );
            case operation_kind::portal_create:    return ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy ) | ::zxfoundation::dgp::right::share;
            case operation_kind::strand_create:    return ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy ) | ::zxfoundation::dgp::right::sched;
            case operation_kind::address_space_create: return ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy );
            case operation_kind::ipc_endpoint_create: return ::zxfoundation::dgp::right::signal;
            case operation_kind::ipc_endpoint_destroy: return ::zxfoundation::dgp::right::signal;
            case operation_kind::ipc_send: return ::zxfoundation::dgp::right::signal;
            case operation_kind::ipc_receive: return ::zxfoundation::dgp::right::signal;
            case operation_kind::shm_export: return ::zxfoundation::dgp::right::share;
            case operation_kind::shm_import: return ::zxfoundation::dgp::right::map;
            case operation_kind::shm_map: return ::zxfoundation::dgp::right::map | ::zxfoundation::dgp::right::share;
            case operation_kind::policy_server_register: return ::zxfoundation::dgp::right::naming_service;
            case operation_kind::policy_server_unregister: return ::zxfoundation::dgp::right::naming_service;

            case operation_kind::io_bind:          return ( ::zxfoundation::dgp::right::bind | ::zxfoundation::dgp::right::submit | ::zxfoundation::dgp::right::observe );
            case operation_kind::io_unbind:        return ( ::zxfoundation::dgp::right::bind | ::zxfoundation::dgp::right::submit | ::zxfoundation::dgp::right::observe );
            case operation_kind::io_submit:        return ( ::zxfoundation::dgp::right::bind | ::zxfoundation::dgp::right::submit | ::zxfoundation::dgp::right::observe );
            case operation_kind::io_dma_extent:    return ( ::zxfoundation::dgp::right::bind | ::zxfoundation::dgp::right::submit | ::zxfoundation::dgp::right::observe );
            case operation_kind::io_completion_dequeue: return ( ::zxfoundation::dgp::right::bind | ::zxfoundation::dgp::right::submit | ::zxfoundation::dgp::right::observe );

            case operation_kind::signal_register:  return ::zxfoundation::dgp::right::signal;
            case operation_kind::signal_poll:      return ::zxfoundation::dgp::right::signal;
            case operation_kind::signal_ack:       return ::zxfoundation::dgp::right::signal;
            case operation_kind::signal_send:      return ::zxfoundation::dgp::right::signal;
            case operation_kind::signal_mask_set:  return ::zxfoundation::dgp::right::signal;
            case operation_kind::signal_mask_get:   return ::zxfoundation::dgp::right::signal;
            case operation_kind::signal_wait:       return ::zxfoundation::dgp::right::signal;

            case operation_kind::mem_alloc:        return ::zxfoundation::dgp::right::map;
            case operation_kind::mem_free:         return ::zxfoundation::dgp::right::map;
            case operation_kind::mem_map:          return ::zxfoundation::dgp::right::map;
            case operation_kind::mem_unmap:        return ::zxfoundation::dgp::right::map;
            case operation_kind::mem_protect:      return ::zxfoundation::dgp::right::map;

            case operation_kind::gate_entry:       return ::zxfoundation::dgp::right::call;
            case operation_kind::gate_query:       return ::zxfoundation::dgp::right::none;

            case operation_kind::cap_acquire:      return ::zxfoundation::dgp::right::none;
            case operation_kind::cap_release:      return ::zxfoundation::dgp::right::none;
            case operation_kind::cap_validate:     return ::zxfoundation::dgp::right::none;
            case operation_kind::cap_revoke:       return ::zxfoundation::dgp::right::delegate | ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy );

            case operation_kind::time_yield:       return ::zxfoundation::dgp::right::none;
            case operation_kind::time_deadline:    return ::zxfoundation::dgp::right::none;
            case operation_kind::time_now:         return ::zxfoundation::dgp::right::none;
            case operation_kind::time_sleep:       return ::zxfoundation::dgp::right::none;
            case operation_kind::strand_join:      return ::zxfoundation::dgp::right::none;
            case operation_kind::strand_exit:      return ::zxfoundation::dgp::right::none;
            case operation_kind::console_write:    return ::zxfoundation::dgp::right::console;
            case operation_kind::register_service: return ::zxfoundation::dgp::right::naming_service;
            case operation_kind::lookup_service:   return ::zxfoundation::dgp::right::naming_service;
            case operation_kind::unregister_service:return ::zxfoundation::dgp::right::naming_service | ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy );

            case operation_kind::domain_exit:      return ::zxfoundation::dgp::right::none;
            case operation_kind::domain_info:      return ::zxfoundation::dgp::right::none;
            case operation_kind::domain_query:     return ::zxfoundation::dgp::right::debug;

            case operation_kind::strand_set_policy:   return ::zxfoundation::dgp::right::sched;
            case operation_kind::strand_set_affinity: return ::zxfoundation::dgp::right::sched;
            case operation_kind::strand_query_state:  return ::zxfoundation::dgp::right::sched | ::zxfoundation::dgp::right::debug;

            case operation_kind::none:                return ::zxfoundation::dgp::right::none;
        }
        return ::zxfoundation::dgp::right::none;
    }

    // Validate operation-specific metadata beyond the common caller/target check.
    [[nodiscard]] auto validate_op_metadata(
        operation_kind op,
        const authorization_metadata& meta
    ) noexcept -> std::expected<void, lib::kernel_error>
    {
        switch (op) {
            // Operations that require a valid VA range:
            case operation_kind::portal_create:
            case operation_kind::mem_map:
            case operation_kind::mem_unmap:
            case operation_kind::mem_protect:
            case operation_kind::mem_free:
            case operation_kind::shm_export:
                if (meta.target_va_start >= meta.target_va_end) {
                    return std::unexpected(
                        lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                break;

                // Operations that require a non-zero gate_id:
            case operation_kind::gate_entry:
                if (meta.gate_id == 0U) {
                    return std::unexpected(
                        lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                break;

                // Operations with no extra metadata to validate:
            case operation_kind::domain_create:
            case operation_kind::domain_exec:
            case operation_kind::domain_destroy:
            case operation_kind::domain_exit:
            case operation_kind::domain_info:
            case operation_kind::domain_query:
            case operation_kind::strand_create:
            case operation_kind::address_space_create:
            case operation_kind::ipc_endpoint_create:
            case operation_kind::ipc_endpoint_destroy:
            case operation_kind::gate_query:
            case operation_kind::ipc_send:
            case operation_kind::ipc_receive:
            case operation_kind::shm_import:
            case operation_kind::shm_map:
            case operation_kind::policy_server_register:
            case operation_kind::policy_server_unregister:
            case operation_kind::gate_create:
            case operation_kind::gate_destroy:
            case operation_kind::portal_destroy:
            case operation_kind::io_bind:
            case operation_kind::io_unbind:
            case operation_kind::io_submit:
            case operation_kind::io_dma_extent:
            case operation_kind::io_completion_dequeue:
            case operation_kind::signal_register:
            case operation_kind::signal_poll:
            case operation_kind::signal_ack:
            case operation_kind::signal_send:
            case operation_kind::signal_mask_set:
            case operation_kind::signal_mask_get:
            case operation_kind::signal_wait:
            case operation_kind::strand_set_policy:
            case operation_kind::strand_set_affinity:
            case operation_kind::strand_query_state:
            case operation_kind::cap_revoke:
            case operation_kind::mem_alloc:
            case operation_kind::cap_acquire:
            case operation_kind::cap_release:
            case operation_kind::cap_validate:
            case operation_kind::time_yield:
            case operation_kind::time_deadline:
            case operation_kind::time_now:
            case operation_kind::time_sleep:
            case operation_kind::strand_join:
            case operation_kind::strand_exit:
            case operation_kind::console_write:
            case operation_kind::register_service:
            case operation_kind::lookup_service:
            case operation_kind::unregister_service:
            case operation_kind::none:
                break;
        }
        return {};
    }
}

namespace zxfoundation::dgp {

    /// @brief Unified DGP authorization chokepoint.
    /// @param[in] caller  Domain initiating the operation.
    /// @param[in] target  Domain being acted upon.
    /// @param[in] op      Operation kind.
    /// @param[in] meta    Additional metadata (rights, VAs, gate IDs).
    /// @return Success when both domains are active and the caller holds
    ///         the requested isolation rights.
    [[nodiscard]] auto authorize(
        const zxfoundation::dgp::domain::domain& caller,
        const zxfoundation::dgp::domain::domain& target,
        operation_kind op,
        const authorization_metadata& meta
    ) noexcept -> std::expected<authorization_lease, lib::kernel_error>
    {
        auto common_result = detail::validate_common(caller, target);
        if (!common_result) {
            return std::unexpected(common_result.error());
        }

        auto op_metadata_result = detail::validate_op_metadata(op, meta);
        if (!op_metadata_result) {
            return std::unexpected(op_metadata_result.error());
        }

        const ::zxfoundation::dgp::right_mask required = detail::required_rights_for_op(op);
        if (required != ::zxfoundation::dgp::right::none) {
            const ::zxfoundation::dgp::right_mask effective = meta.requested_rights | required;
            if (!has_all_rights(caller.granted_rights, effective)) {
                return std::unexpected(
                    lib::kernel_error::from_generic(lib::generic_error::denied));
            }
        }

        if (meta.token) {
            auto cap_r = zxfoundation::dgp::cap::cap_validate_generic_for(
                meta.token, meta.token_rights, caller.ko.id);
            if (!cap_r) {
                return std::unexpected(cap_r.error());
            }
        }

        if (auto kind = detail::policy_kind_for_op(op)) {
            if (zxfoundation::sched::context::nesting_depth() == 0U &&
                !zxfoundation::sched::context::preempt_disabled()) {
                const auto consult = zxfoundation::dgp::policy::decide(
                    caller.ko.id, kind.value(),
                    static_cast<u64>(meta.requested_rights),
                    static_cast<u64>(op), policy::DECIDE_TIMEOUT_NS);
                if (consult.outcome == dgp::policy::decide_outcome::deny) {
                    return std::unexpected(lib::kernel_error::from_generic(
                        lib::generic_error::denied));
                }
            }
        }

        return authorization_lease{
            &caller.ko, &target.ko, caller.ko.generation, target.ko.generation,
            op, required, false};
    }

} // namespace zxfoundation::dgp
