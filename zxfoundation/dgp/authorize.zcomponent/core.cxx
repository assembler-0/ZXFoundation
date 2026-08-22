/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/authorize.zcomponent/core.cxxm
/// @brief Unified DGP authorization.

module zxfoundation.dgp.authorize.core;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.sync_contract.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.scoms.kobject.base;
import lib.error;
import std;

namespace zxfoundation::dgp::detail {
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
        -> dgp::isolation::isolation_rights
    {
        switch (op) {
            case operation_kind::domain_create:    return dgp::isolation::isolation_rights::lifecycle;
            case operation_kind::domain_destroy:   return dgp::isolation::isolation_rights::lifecycle;
            case operation_kind::gate_create:      return dgp::isolation::isolation_rights::lifecycle;
            case operation_kind::gate_destroy:     return dgp::isolation::isolation_rights::lifecycle;
            case operation_kind::portal_destroy:   return dgp::isolation::isolation_rights::lifecycle;
            case operation_kind::portal_create:    return dgp::isolation::isolation_rights::lifecycle | dgp::isolation::isolation_rights::share;
            case operation_kind::strand_create:    return dgp::isolation::isolation_rights::lifecycle | dgp::isolation::isolation_rights::sched;
            case operation_kind::address_space_create: return dgp::isolation::isolation_rights::lifecycle;
            case operation_kind::ipc_endpoint_create: return dgp::isolation::isolation_rights::signal;
            case operation_kind::ipc_endpoint_destroy: return dgp::isolation::isolation_rights::signal;
            case operation_kind::ipc_send: return dgp::isolation::isolation_rights::signal;
            case operation_kind::ipc_receive: return dgp::isolation::isolation_rights::signal;
            case operation_kind::shm_export: return dgp::isolation::isolation_rights::share;
            case operation_kind::shm_import: return dgp::isolation::isolation_rights::map;
            case operation_kind::shm_map: return dgp::isolation::isolation_rights::map | dgp::isolation::isolation_rights::share;
            case operation_kind::policy_server_register: return dgp::isolation::isolation_rights::service;
            case operation_kind::policy_server_unregister: return dgp::isolation::isolation_rights::service;

            case operation_kind::io_bind:          return dgp::isolation::isolation_rights::io;
            case operation_kind::io_unbind:        return dgp::isolation::isolation_rights::io;
            case operation_kind::io_submit:        return dgp::isolation::isolation_rights::io;
            case operation_kind::io_irq_bind:      return dgp::isolation::isolation_rights::io;
            case operation_kind::io_dma_extent:    return dgp::isolation::isolation_rights::io;
            case operation_kind::io_completion_dequeue: return dgp::isolation::isolation_rights::io;

            case operation_kind::signal_register:  return dgp::isolation::isolation_rights::signal;
            case operation_kind::signal_poll:      return dgp::isolation::isolation_rights::signal;
            case operation_kind::signal_ack:       return dgp::isolation::isolation_rights::signal;
            case operation_kind::signal_send:      return dgp::isolation::isolation_rights::signal;
            case operation_kind::signal_mask_set:  return dgp::isolation::isolation_rights::signal;
            case operation_kind::signal_mask_get:   return dgp::isolation::isolation_rights::signal;
            case operation_kind::signal_wait:       return dgp::isolation::isolation_rights::signal;

            case operation_kind::mem_alloc:        return dgp::isolation::isolation_rights::map;
            case operation_kind::mem_free:         return dgp::isolation::isolation_rights::map;
            case operation_kind::mem_map:          return dgp::isolation::isolation_rights::map;
            case operation_kind::mem_unmap:        return dgp::isolation::isolation_rights::map;
            case operation_kind::mem_protect:      return dgp::isolation::isolation_rights::map;
            case operation_kind::mem_share:        return dgp::isolation::isolation_rights::share;

            case operation_kind::gate_entry:       return dgp::isolation::isolation_rights::enter;

            case operation_kind::cap_acquire:      return dgp::isolation::isolation_rights::none;
            case operation_kind::cap_release:      return dgp::isolation::isolation_rights::none;
            case operation_kind::cap_validate:     return dgp::isolation::isolation_rights::none;
            case operation_kind::cap_revoke:       return dgp::isolation::isolation_rights::delegate | dgp::isolation::isolation_rights::lifecycle;

            case operation_kind::time_yield:       return dgp::isolation::isolation_rights::none;
            case operation_kind::time_deadline:    return dgp::isolation::isolation_rights::none;
            case operation_kind::time_now:         return dgp::isolation::isolation_rights::none;
            case operation_kind::console_write:    return dgp::isolation::isolation_rights::console;
            case operation_kind::register_service: return dgp::isolation::isolation_rights::service;
            case operation_kind::lookup_service:   return dgp::isolation::isolation_rights::service;
            case operation_kind::unregister_service:return dgp::isolation::isolation_rights::service | dgp::isolation::isolation_rights::lifecycle;

            case operation_kind::domain_exit:      return dgp::isolation::isolation_rights::none;
            case operation_kind::domain_info:      return dgp::isolation::isolation_rights::none;
            case operation_kind::domain_query:     return dgp::isolation::isolation_rights::debug;

            case operation_kind::strand_set_policy:   return dgp::isolation::isolation_rights::sched;
            case operation_kind::strand_set_affinity: return dgp::isolation::isolation_rights::sched;
            case operation_kind::strand_query_state:  return dgp::isolation::isolation_rights::sched | dgp::isolation::isolation_rights::debug;

            case operation_kind::none:                return dgp::isolation::isolation_rights::none;
        }
        return dgp::isolation::isolation_rights::none;
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
            case operation_kind::mem_share:
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
            case operation_kind::io_irq_bind:
                if (meta.gate_id == 0U) {
                    return std::unexpected(
                        lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                break;

                // Operations with no extra metadata to validate:
            case operation_kind::domain_create:
            case operation_kind::domain_destroy:
            case operation_kind::domain_exit:
            case operation_kind::domain_info:
            case operation_kind::domain_query:
            case operation_kind::strand_create:
            case operation_kind::address_space_create:
            case operation_kind::ipc_endpoint_create:
            case operation_kind::ipc_endpoint_destroy:
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

        const dgp::isolation::isolation_rights required = detail::required_rights_for_op(op);
        if (required != dgp::isolation::isolation_rights::none) {
            const dgp::isolation::isolation_rights effective = meta.requested_rights | required;
            if (!has_all_rights(caller.granted_rights, effective)) {
                return std::unexpected(
                    lib::kernel_error::from_memory(lib::memory_error::capability_invalid));
            }
        }

        return authorization_lease{
            &caller.ko, &target.ko, caller.ko.generation, target.ko.generation,
            op, required, false};
    }

} // namespace zxfoundation::dgp
