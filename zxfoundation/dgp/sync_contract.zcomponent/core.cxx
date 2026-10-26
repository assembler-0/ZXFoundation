/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/sync_contract.zcomponent/core.cxx
/// @brief DGP synchronization-contract typestate and validation implementation.

module zxfoundation.dgp.sync_contract.core;
import zxfoundation.base.types;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.irq;
import lib.error;
import lib.stl;

namespace zxfoundation::dgp::sync_contract {

    /// @brief Capture the current CPU synchronization state for a DGP call-site check.
    /// @param[in] context Synchronization context expected by the caller.
    /// @param[in] phase Domain teardown phase associated with the operation.
    /// @param[in] domain_id Domain whose authority or lifecycle state is being checked.
    /// @return Snapshot containing CPU ID, qspinlock depth, interrupt mask state, and RCU read-side state.
    [[nodiscard]] auto contract_snapshot(
        lock_context context,
        teardown_phase phase,
        u32 domain_id
    ) noexcept -> contract_state {
        const auto& pcp = arch::s390x::cpu::percpu::this_cpu();

        contract_state state{};
        state.domain_id = domain_id;
        state.cpu_id = pcp.cpu_id;
        state.lock_depth = static_cast<u16>(pcp.lock_depth);
        state.irqs_disabled = !arch::s390x::cpu::irq::irqs_enabled();
        state.rcu_read_side = (pcp.in_rcu_read_side != 0U);
        state.context = context;
        state.phase = phase;
        return state;
    }

    /// @brief Validate an IRQ-state precondition.
    /// @param[in] state Current synchronization state.
    /// @param[in] contract Required interrupt-state contract.
    /// @return Success when the current state satisfies the required interrupt condition.
    [[nodiscard]] auto validate_irq_state(
        const contract_state& state,
        irq_contract contract
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (contract == irq_contract::must_be_disabled && !state.irqs_disabled) {
            return std::unexpected(lib::kernel_error::from_locking(lib::locking_error::not_locked));
        }

        if (contract == irq_contract::must_be_enabled && state.irqs_disabled) {
            return std::unexpected(lib::kernel_error::from_locking(lib::locking_error::already_locked));
        }

        return {};
    }

    /// @brief Validate one exact teardown-phase transition.
    /// @param[in] current Current teardown phase.
    /// @param[in] requested Requested teardown phase.
    /// @return Success when @p requested is the immediate successor of @p current.
    [[nodiscard]] auto validate_teardown_phase(
        teardown_phase current,
        teardown_phase requested
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const bool is_next =
            (current == teardown_phase::none && requested == teardown_phase::deny_new_entries)
            || (current == teardown_phase::deny_new_entries && requested == teardown_phase::drain_gates)
            || (current == teardown_phase::drain_gates && requested == teardown_phase::drain_portals)
            || (current == teardown_phase::drain_portals && requested == teardown_phase::release_memory)
            || (current == teardown_phase::release_memory && requested == teardown_phase::release_identity);
        if (!is_next) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        return {};
    }

    /// @brief Advance a domain-owned teardown phase by exactly one stage.
    /// @param[in,out] current Stored phase owned by the domain being destroyed.
    /// @param[in] expected Phase the domain must currently hold.
    /// @param[in] requested Immediate phase to record.
    /// @return Success when the stored phase was advanced.
    [[nodiscard]] auto advance_teardown_phase(
        teardown_phase& current,
        teardown_phase expected,
        teardown_phase requested
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (current != expected) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        auto transition = validate_teardown_phase(expected, requested);
        if (!transition) return std::unexpected(transition.error());
        current = requested;
        return {};
    }

    /// @brief Validate that a DGP authority lookup may run in the supplied context.
    /// @param[in] state Current synchronization state.
    /// @return Success when the context is safe for non-blocking authority validation.
    [[nodiscard]] auto validate_authority_lookup(
        const contract_state& state
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (state.context == lock_context::teardown && state.phase == teardown_phase::release_identity) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        return {};
    }

    static_assert(sizeof(lock_context) == sizeof(u8), "DGP lock context must remain byte-sized");
    static_assert(sizeof(irq_contract) == sizeof(u8), "DGP IRQ contract must remain byte-sized");
    static_assert(static_cast<u8>(teardown_phase::deny_new_entries) < static_cast<u8>(teardown_phase::release_identity), "DGP teardown phases must remain ordered");


} // namespace zxfoundation::dgp::sync_contract
