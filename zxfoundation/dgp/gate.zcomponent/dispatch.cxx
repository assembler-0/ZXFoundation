/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/gate.zcomponent/dispatch.cxx
/// @brief Gate subsystem dispatch table implementation

module zxfoundation.dgp.gate.dispatch;
import zxfoundation.dgp.gate.types;
import zxfoundation.dgp.gate.core;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.dgp.cap.core;
import zxfoundation.dgp.cap.types;
import zxfoundation.dgp.authorize;
import zxfoundation.dgp.domain.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.isolation;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.sync_contract;
import zxfoundation.sync.lockable;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sys.printk.core;
import lib.error;
import lib.stl;

namespace zxfoundation::dgp::gate::dispatch::detail {

    using zxfoundation::dgp::cap::cap_token;
    using zxfoundation::dgp::cap::cap_rights;
    using zxfoundation::dgp::isolation::isolation_rights;
    using zxfoundation::dgp::sync_contract::lock_context;
    using zxfoundation::dgp::sync_contract::teardown_phase;
    using zxfoundation::base::lifecycle_state;
    using zxfoundation::dgp::gate::gate_desc;
    using zxfoundation::dgp::gate::gate_auth;
    using zxfoundation::dgp::isolation::has_all_rights;

    struct entry {
        dispatch_handler handler{nullptr};
        entry_meta meta{};
    };

    static entry s_table[DISPATCH_TABLE_SIZE]{};
    static sync::qspinlock::qspinlock s_lock{};

    [[nodiscard]] auto acquire_caller_domain() noexcept
        -> zxfoundation::dgp::domain::domain * {
        return zxfoundation::dgp::domain::current_domain_or_nucleus();
    }

    [[nodiscard]] auto dgp_rights_for(gate_auth_level auth) noexcept
        -> dgp::isolation::isolation_rights {
        switch (auth) {
            case gate_auth_level::none:
                return dgp::isolation::isolation_rights::enter;
            case gate_auth_level::capability:
                return dgp::isolation::isolation_rights::enter;
            case gate_auth_level::admin:
                return dgp::isolation::isolation_rights::enter | dgp::isolation::isolation_rights::lifecycle;
            case gate_auth_level::domain_map:
                return dgp::isolation::isolation_rights::enter | dgp::isolation::isolation_rights::map;
            case gate_auth_level::storage_key:
                return dgp::isolation::isolation_rights::enter | dgp::isolation::isolation_rights::share;
            case gate_auth_level::io:
                return dgp::isolation::isolation_rights::enter | dgp::isolation::isolation_rights::io;
            case gate_auth_level::signal:
                return dgp::isolation::isolation_rights::enter | dgp::isolation::isolation_rights::signal;
        }
        return dgp::isolation::isolation_rights::enter;
    }

    /// @brief Full DGP gate validation — checks gate state, rights, and auth.
    /// @param gate       Pointer to the gate_desc (must be non-null).
    /// @return true if authorized.
    [[nodiscard]] auto validate_gate_entry(
        u32 caller_domain_id,
        zxfoundation::dgp::gate::gate_desc* gate,
        dgp::isolation::isolation_rights requested
    ) noexcept -> bool {
        // Check gate is active
        if (gate->ko.state != lifecycle_state::active) return false;

        auto* caller_domain = zxfoundation::dgp::domain::domain_find(caller_domain_id);
        auto* target_domain = zxfoundation::dgp::domain::domain_find(gate->target_domain);
        if (!caller_domain || !target_domain) return false;

        auto result = zxfoundation::dgp::authorize(
            *caller_domain, *target_domain,
            zxfoundation::dgp::operation_kind::gate_entry,
            zxfoundation::dgp::authorization_metadata
                { .requested_rights = requested, .gate_id = gate->ko.id }
        );
        if (!result) return false;

        ++target_domain->gate_count;
        return true;
    }

    /// @brief Resolve the *target* domain for an op (the domain being acted
    ///        upon, distinct from the caller).
    [[nodiscard]] auto target_domain_for_op(
        zxfoundation::dgp::operation_kind op_kind,
        zxfoundation::dgp::domain::domain& caller,
        u64 a0, u64 a1
    ) noexcept -> zxfoundation::dgp::domain::domain* {
        using zxfoundation::dgp::operation_kind;
        switch (op_kind) {
            // Cross-domain ops (a1 carries the target domain id for most):
            case operation_kind::mem_share:
            case operation_kind::portal_create:
            case operation_kind::portal_destroy:
            case operation_kind::domain_destroy: {
                const auto tid = static_cast<zxfoundation::dgp::domain::domain_id>(a1);
                if (tid == 0) return zxfoundation::dgp::domain::domain_nucleus();
                return zxfoundation::dgp::domain::domain_find(tid);
            }
            // signal_send and domain_query carry the target domain id in a0:
            case operation_kind::signal_send: {
                const auto tid = static_cast<zxfoundation::dgp::domain::domain_id>(a0);
                if (tid == 0) return zxfoundation::dgp::domain::domain_nucleus();
                return zxfoundation::dgp::domain::domain_find(tid);
            }
            case operation_kind::domain_query: {
                const auto tid = static_cast<zxfoundation::dgp::domain::domain_id>(a0);
                if (tid == 0 || tid == caller.ko.id) return &caller;
                return zxfoundation::dgp::domain::domain_find(tid);
            }

            case operation_kind::strand_create: {
                const auto owner = static_cast<zxfoundation::dgp::domain::domain_id>(a1 & 0xFFFFU);
                if (owner == 0 || owner == caller.ko.id) return &caller;
                return zxfoundation::dgp::domain::domain_find(owner);
            }
            // Everything else targets the caller's own domain.
            default:
                return &caller;
        }
    }

    /// @brief Extract operation-specific authorization metadata (VA range,
    ///        gate id, requested-rights hint) from the syscall arguments.
    [[nodiscard]] auto metadata_for_op(
        zxfoundation::dgp::operation_kind op_kind,
        u64 a0, u64 a1, u64 a2, u64 a3
    ) noexcept -> zxfoundation::dgp::authorization_metadata {
        using zxfoundation::dgp::operation_kind;
        zxfoundation::dgp::authorization_metadata meta{};

        switch (op_kind) {
            // VA-bounded ops.  The DGP invariant is start < end.
            case operation_kind::mem_map: {
                meta.target_va_start = a1;
                meta.target_va_end   = a1 + a3;
                break;
            }
            case operation_kind::mem_unmap:
            case operation_kind::mem_protect:
            case operation_kind::mem_free: {
                meta.target_va_start = a0;
                meta.target_va_end   = a0 + a1;
                break;
            }
            case operation_kind::mem_share:
            case operation_kind::portal_create: {
                meta.target_va_start = a2;
                meta.target_va_end   = a3;
                break;
            }
            case operation_kind::shm_export: {
                meta.target_va_start = a0;
                meta.target_va_end   = a0 + a1;
                break;
            }

            // Gate-id carrying ops.
            case operation_kind::gate_entry:
                meta.gate_id = static_cast<u32>(a0);
                break;
            case operation_kind::io_irq_bind:
                meta.gate_id = static_cast<u32>(a1);
                break;

            default:
                break;
        }
        return meta;
    }

    /// @brief Validate DGP contract + capability before dispatch.
    /// @return true if the call is authorized.
    [[nodiscard]] auto validate_call(
        u32 packed_nr,
        u64 a0, u64 a1, u64 a2, u64 a3,
        const entry &ent,
        zxfoundation::dgp::domain::domain *caller
    ) noexcept -> bool {
        if (!caller) [[unlikely]] return false;

        const auto sync_state = dgp::sync_contract::contract_snapshot(
            lock_context::thread, teardown_phase::none, caller->ko.id);
        auto sync_r = dgp::sync_contract::validate_authority_lookup(sync_state);
        if (!sync_r) {
            sys::printk::perr("dispatch: call {} — DGP sync contract failed",
                              packed_nr);
            return false;
        }

        if (ent.meta.op_kind != zxfoundation::dgp::operation_kind::none) {
            auto* target = target_domain_for_op(
                ent.meta.op_kind, *caller, a0, a1);
            if (!target) {
                sys::printk::perr("dispatch: call {} — target domain not found",
                                  packed_nr);
                return false;
            }
            auto meta = metadata_for_op(ent.meta.op_kind, a0, a1, a2, a3);
            auto dgp_r = zxfoundation::dgp::authorize(
                *caller, *target, ent.meta.op_kind, meta);
            if (!dgp_r) {
                sys::printk::perr("dispatch: call {} — DGP authorize denied (op_kind={})",
                                  packed_nr,
                                  static_cast<u32>(ent.meta.op_kind));
                return false;
            }
        }

        if (ent.meta.auth != gate_auth_level::none) {
            if (auto* gate = zxfoundation::dgp::gate::gate_find(packed_nr)) {
                if (!validate_gate_entry(caller->ko.id, gate,
                                         dgp_rights_for(ent.meta.auth))) {
                    sys::printk::perr("dispatch: call {} — DGP gate validation failed",
                                      packed_nr);
                    return false;
                }
            }
        }

        switch (ent.meta.auth) {
            case gate_auth_level::none:
            case gate_auth_level::storage_key:
                break;

            case gate_auth_level::capability: {
                auto cap_r = dgp::cap::cap_validate_generic_for(
                    cap_token{a0}, cap_rights::none, caller->ko.id);
                if (!cap_r) {
                    sys::printk::perr("dispatch: call {} — capability validation failed",
                                      packed_nr);
                    return false;
                }
                break;
            }

            case gate_auth_level::admin: {
                auto cap_r = dgp::cap::cap_validate_generic_for(
                    cap_token{a0}, cap_rights::admin, caller->ko.id);
                if (!cap_r) {
                    sys::printk::perr("dispatch: call {} — admin cap validation failed",
                                      packed_nr);
                    return false;
                }
                break;
            }

            case gate_auth_level::domain_map: {
                auto cap_r = dgp::cap::cap_validate_generic_for(
                    cap_token{a0}, cap_rights::map, caller->ko.id);
                if (!cap_r) {
                    sys::printk::perr("dispatch: call {} — domain_map cap validation failed",
                                      packed_nr);
                    return false;
                }
                break;
            }

            case gate_auth_level::io:
            case gate_auth_level::signal:
                break;
        }

        return true;
    }

} // namespace zxfoundation::dgp::gate::dispatch::detail

namespace zxfoundation::dgp::gate::dispatch {

    using zxfoundation::dgp::domain::domain;
    using zxfoundation::dgp::cap::cap_token;
    using zxfoundation::dgp::cap::cap_rights;
    using zxfoundation::dgp::isolation::isolation_rights;
    using zxfoundation::dgp::sync_contract::lock_context;
    using zxfoundation::dgp::sync_contract::teardown_phase;

    /// @brief Register a handler at a given packed call slot.
    /// @param packed_nr  Table index: (namespace << 5) | operation.
    /// @param handler    The handler function.
    /// @param meta       Authorization metadata.
    /// @return Success, or kernel_error if slot occupied or out of range.
    [[nodiscard]] auto register_handler(
        u32 packed_nr,
        dispatch_handler handler,
        const entry_meta &meta
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (packed_nr >= DISPATCH_TABLE_SIZE) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::out_of_range));
        }
        if (!handler) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        const sync::irq_lock_guard guard{detail::s_lock};

        if (detail::s_table[packed_nr].handler) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::internal_error));
        }

        detail::s_table[packed_nr].handler = handler;
        detail::s_table[packed_nr].meta = meta;

        return {};
    }

    /// @brief Bulk-register all handlers for a namespace.
    /// @param ns      The namespace whose handlers to register.
    /// @param ops     Array of (operation_id, handler, meta) tuples.
    /// @param count   Number of entries in @p ops.
    /// @return Success, or the first registration error encountered.

    [[nodiscard]] auto register_namespace_handlers(
        call_namespace ns,
        const registration_op ops[],
        u32 count
    ) noexcept -> std::expected<void, lib::kernel_error> {
        for (u32 i = 0; i < count; ++i) {
            auto r = register_handler(
                pack_index(ns, ops[i].operation_id),
                ops[i].handler,
                ops[i].meta);
            if (!r) return r;
        }
        return {};
    }

    /// @brief Dispatch a nucleus call through the table.
    /// @param packed_nr  Table index.
    /// @param a0-a4      Arguments.
    /// @return Handler return value, or ~0ULL on error.
    [[nodiscard]] auto dispatch(
        u32 packed_nr,
        u64 a0, u64 a1, u64 a2, u64 a3, u64 a4
    ) noexcept -> u64 {
        if (packed_nr >= DISPATCH_TABLE_SIZE) [[unlikely]] {
            sys::printk::perr("dispatch: packed_nr {} out of range", packed_nr);
            return ~0ULL;
        }

        const auto &ent = detail::s_table[packed_nr];
        if (!ent.handler) [[unlikely]] {
            sys::printk::perr("dispatch: no handler for packed_nr {}", packed_nr);
            return ~0ULL;
        }

        auto *caller = detail::acquire_caller_domain();
        if (!detail::validate_call(packed_nr, a0, a1, a2, a3, ent, caller)) [[unlikely]] {
            return ~0ULL;
        }

        return ent.handler(a0, a1, a2, a3, a4);
    }

    /// @brief Return true if the given slot has a registered handler.
    [[nodiscard]] auto is_registered(u32 packed_nr) noexcept -> bool {
        if (packed_nr >= DISPATCH_TABLE_SIZE) return false;
        return detail::s_table[packed_nr].handler != nullptr;
    }

} // namespace zxfoundation::dgp::gate::dispatch_table