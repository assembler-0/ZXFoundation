/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/cap.zcomponent/core.cxx
/// @brief Capability management — acquire, release, derive, and validate
///        capability tokens for the DGP kernel's authorization model.

module zxfoundation.dgp.cap.core;
import zxfoundation.dgp.cap.types;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.table;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.gate.types;
import zxfoundation.dgp.portal.types;
import zxfoundation.memory.vm.types;
import zxfoundation.memory.objects;
import zxfoundation.sys.zxio.types;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.memory.kmalloc;
import lib.error;
import lib.format;
import lib.stl;

namespace zxfoundation::dgp::cap::detail {

    /// @brief Maximum number of lookup table entries (must exceed largest cap_type).
    no_export constexpr u32 LOOKUP_TABLE_SIZE = 16;
    no_export constexpr u32 MAX_CAPABILITIES = 1024;

    struct capability_slot {
        scoms::kobject::bobject* target{nullptr};
        cap_type        type{cap_type::none};
        ::zxfoundation::dgp::right_mask      rights{::zxfoundation::dgp::right::none};
        u32             owner_domain{~0U};
        u32             target_generation{0};
        u16             generation{1};
        bool            active{false};
    };

    no_export capability_slot* s_capabilities{nullptr};
    no_export sync::qspinlock::qspinlock<sync::lock_rank::cap_table> s_capability_lock{};

    /// @brief Callback table indexed by cap_type.
    no_export lookup_fn s_lookup_table[LOOKUP_TABLE_SIZE]{};

    [[nodiscard]] no_export auto ensure_capabilities_locked() noexcept -> bool {
        if (s_capabilities) return true;
        s_capabilities = static_cast<capability_slot*>(
            memory::kmalloc::kmalloc(sizeof(capability_slot) * MAX_CAPABILITIES));
        if (!s_capabilities) return false;
        for (u32 i = 0; i < MAX_CAPABILITIES; ++i) s_capabilities[i] = {};
        return true;
    }

    /// @brief Retrieve a kobject* and its generation via the callback table.
    /// @param[in]  type    cap_type discriminator.
    /// @param[in]  obj_id  Object ID.
    /// @return  Pair of (kobject*, generation) or error.
    [[nodiscard]] no_export auto lookup_kobject(
        cap_type type, u16 obj_id
    ) noexcept -> std::expected<std::pair<scoms::kobject::bobject*, u32>, lib::kernel_error> {

        const auto idx = static_cast<u32>(type);
        if (idx >= LOOKUP_TABLE_SIZE || !s_lookup_table[idx]) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_supported));
        }

        auto* ko = s_lookup_table[idx](obj_id);
        if (!ko) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));
        }
        return std::pair<scoms::kobject::bobject*, u32>{ko, ko->generation};
    }

    /// @brief Validate that the token's type has sufficient rights for an operation.
    [[nodiscard]] no_export auto check_rights(cap_token token, ::zxfoundation::dgp::right_mask needed) noexcept
        -> std::expected<void, lib::kernel_error>
    {
        if (!::zxfoundation::dgp::token_covers(token.rights(), needed)) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        return {};
    }

    [[nodiscard]] no_export auto slot_for(cap_token token) noexcept -> capability_slot* {
        if (!s_capabilities) return nullptr;
        if (!token || token.object_id() == 0U ||
            static_cast<u32>(token.object_id()) >= MAX_CAPABILITIES) {
            return nullptr;
        }
        auto& slot = s_capabilities[token.object_id()];
        if (!slot.active || slot.generation != token.generation() ||
            slot.type != token.type() ||
            slot.rights != ::zxfoundation::dgp::carried_rights(token.rights())) {
            return nullptr;
        }
        return &slot;
    }

    [[nodiscard]] no_export auto issue_locked(
        cap_type type, u16 obj_id, ::zxfoundation::dgp::right_mask rights, u32 owner_domain
    ) noexcept -> std::expected<cap_token, lib::kernel_error> {
        if (!ensure_capabilities_locked()) return std::unexpected(
            lib::kernel_error::from_memory(lib::memory_error::out_of_memory));
        auto result = lookup_kobject(type, obj_id);
        if (!result) return std::unexpected(result.error());

        u32 slot_id = 0;
        for (u32 i = 1; i < MAX_CAPABILITIES; ++i) {
            if (!s_capabilities[i].active) {
                slot_id = i;
                break;
            }
        }
        if (slot_id == 0U) {
            return std::unexpected(
                lib::kernel_error::from_memory(lib::memory_error::out_of_memory));
        }

        auto& slot = s_capabilities[slot_id];
        auto [target, generation] = *result;
        if (target->state != base::lifecycle_state::active) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_supported));
        }
        if (!target->try_get()) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));
        }

        const auto carried = ::zxfoundation::dgp::carried_rights(rights);
        slot.target = target;
        slot.type = type;
        slot.rights = carried;
        slot.owner_domain = owner_domain;
        slot.target_generation = generation;
        slot.active = true;

        return cap_token::make(
            type, static_cast<u16>(slot_id), slot.generation, carried);
    }

    [[nodiscard]] no_export auto validate_slot(
        cap_token token, ::zxfoundation::dgp::right_mask needed, u32 owner_domain, bool check_owner,
        scoms::kobject::bobject*& target
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto* slot = slot_for(token);
        if (!slot || !slot->target ||
            slot->target->generation != slot->target_generation ||
            slot->target->state == base::lifecycle_state::dying ||
            slot->target->state == base::lifecycle_state::destroyed) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));
        }
        if (check_owner && slot->owner_domain != owner_domain) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::denied));
        }
        auto rights_result = check_rights(token, needed);
        if (!rights_result) return std::unexpected(rights_result.error());
        target = slot->target;
        return {};
    }

} // namespace zxfoundation::dgp::cap::detail

namespace zxfoundation::dgp::cap {

    [[nodiscard]] auto cap_issue(
        cap_type type, u16 obj_id, ::zxfoundation::dgp::right_mask rights, u32 owner_domain
    ) noexcept -> std::expected<cap_token, lib::kernel_error> {
        const sync::irq_lock_guard _{detail::s_capability_lock};
        return detail::issue_locked(type, obj_id, rights, owner_domain);
    }

    [[nodiscard]] auto cap_duplicate(
        cap_token source, ::zxfoundation::dgp::right_mask rights, u32 owner_domain
    ) noexcept -> std::expected<cap_token, lib::kernel_error> {
        const sync::irq_lock_guard _{detail::s_capability_lock};
        scoms::kobject::bobject* target = nullptr;
        auto valid = detail::validate_slot(
            source, rights, owner_domain, true, target);
        if (!valid) return std::unexpected(valid.error());
        auto* slot = detail::slot_for(source);
        return detail::issue_locked(
            slot->type, static_cast<u16>(target->id), rights, owner_domain);
    }

    [[nodiscard]] auto cap_acquire(
        cap_token source, ::zxfoundation::dgp::right_mask rights, u32 owner_domain
    ) noexcept -> std::expected<cap_token, lib::kernel_error> {
        const sync::irq_lock_guard _{detail::s_capability_lock};
        scoms::kobject::bobject* target = nullptr;
        auto valid = detail::validate_slot(
            source, rights, owner_domain, true, target);
        if (!valid) return std::unexpected(valid.error());
        auto* slot = detail::slot_for(source);
        return detail::issue_locked(
            slot->type, static_cast<u16>(target->id), rights, owner_domain);
    }

    [[nodiscard]] auto cap_duplicate_for(
        cap_token source, ::zxfoundation::dgp::right_mask rights, u32 source_domain,
        u32 owner_domain
    ) noexcept -> std::expected<cap_token, lib::kernel_error> {
        const sync::irq_lock_guard _{detail::s_capability_lock};
        scoms::kobject::bobject* target = nullptr;
        auto valid = detail::validate_slot(
            source, rights, source_domain, true, target);
        if (!valid) return std::unexpected(valid.error());
        auto* slot = detail::slot_for(source);
        if (owner_domain != source_domain
            && (slot->rights & ::zxfoundation::dgp::right::delegate) != ::zxfoundation::dgp::right::delegate) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::denied));
        }
        return detail::issue_locked(
            slot->type, static_cast<u16>(target->id), rights, owner_domain);
    }

    [[nodiscard]] auto cap_release(cap_token token) noexcept
        -> std::expected<void, lib::kernel_error>
    {
        if (!token) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        const sync::irq_lock_guard _{detail::s_capability_lock};
        auto* slot = detail::slot_for(token);
        if (!slot) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));
        }
        auto* ko = slot->target;
        slot->active = false;
        slot->target = nullptr;
        slot->generation = static_cast<u16>(slot->generation + 1U);
        ko->put();

        return {};
    }

    [[nodiscard]] auto cap_validate_generic(
        cap_token token, ::zxfoundation::dgp::right_mask needed
    ) noexcept -> std::expected<void, lib::kernel_error> {

        if (!token) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        const sync::irq_lock_guard _{detail::s_capability_lock};
        scoms::kobject::bobject* target = nullptr;
        return detail::validate_slot(token, needed, 0, false, target);
    }

    [[nodiscard]] auto cap_validate_generic_for(
        cap_token token, ::zxfoundation::dgp::right_mask needed, u32 owner_domain
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const sync::irq_lock_guard _{detail::s_capability_lock};
        scoms::kobject::bobject* target = nullptr;
        return detail::validate_slot(token, needed, owner_domain, true, target);
    }

    [[nodiscard]] auto cap_resolve(
        cap_token token, ::zxfoundation::dgp::right_mask needed
    ) noexcept -> std::expected<scoms::kobject::bobject*, lib::kernel_error> {
        const sync::irq_lock_guard _{detail::s_capability_lock};
        scoms::kobject::bobject* target = nullptr;
        auto result = detail::validate_slot(token, needed, 0, false, target);
        if (!result) return std::unexpected(result.error());
        return target;
    }

    [[nodiscard]] auto register_lookup(
        cap_type   type,
        lookup_fn  fn
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const auto idx = static_cast<u32>(type);
        if (idx >= detail::LOOKUP_TABLE_SIZE) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_supported));
        }
        detail::s_lookup_table[idx] = fn;
        return {};
    }

    [[nodiscard]] auto cap_revoke(cap_token token) noexcept
        -> std::expected<void, lib::kernel_error>
    {
        if (!token) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        const sync::irq_lock_guard _{detail::s_capability_lock};
        auto* slot = detail::slot_for(token);
        if (!slot) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));
        }
        slot->active = false;
        auto* target = slot->target;
        slot->target = nullptr;
        slot->generation = static_cast<u16>(slot->generation + 1U);
        target->put();
        return {};
    }

    [[nodiscard]] auto cap_revoke_for_domain(
        u32 domain_id
    ) noexcept -> std::expected<u32, lib::kernel_error> {
        const sync::irq_lock_guard _{detail::s_capability_lock};
        if (!detail::s_capabilities) return 0U;
        u32 revoked = 0;
        for (u32 i = 1; i < detail::MAX_CAPABILITIES; ++i) {
            auto& slot = detail::s_capabilities[i];
            const bool targets_domain = slot.active && slot.type == cap_type::domain &&
                slot.target && slot.target->id == domain_id;
            if (!slot.active ||
                (slot.owner_domain != domain_id && !targets_domain)) {
                continue;
            }
            auto* target = slot.target;
            slot.active = false;
            slot.target = nullptr;
            slot.generation = static_cast<u16>(slot.generation + 1U);
            if (target) target->put();
            ++revoked;
        }
        return revoked;
    }

    [[nodiscard]] auto cap_revoke_target(
        scoms::kobject::bobject& target
    ) noexcept -> std::expected<u32, lib::kernel_error> {
        const sync::irq_lock_guard _{detail::s_capability_lock};
        u32 revoked = 0;
        for (u32 i = 1; i < detail::MAX_CAPABILITIES; ++i) {
            auto& slot = detail::s_capabilities[i];
            if (!slot.active || slot.target != &target) continue;
            slot.active = false;
            slot.target = nullptr;
            slot.generation = static_cast<u16>(slot.generation + 1U);
            target.put();
            ++revoked;
        }
        return revoked;
    }

    template <>
    [[nodiscard]] auto cap_validate<zxfoundation::dgp::domain::domain>(
        cap_token token, ::zxfoundation::dgp::right_mask needed
    ) noexcept -> std::expected<zxfoundation::dgp::domain::domain*, lib::kernel_error> {
        const sync::irq_lock_guard _{detail::s_capability_lock};
        scoms::kobject::bobject* ko = nullptr;
        auto result = detail::validate_slot(token, needed, 0, false, ko);
        if (!result) return std::unexpected(result.error());
        if (token.type() != cap_type::domain) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        return reinterpret_cast<zxfoundation::dgp::domain::domain*>(ko);
    }

    template <>
    [[nodiscard]] auto cap_validate<zxfoundation::dgp::gate::gate_desc>(
        cap_token token, ::zxfoundation::dgp::right_mask needed
    ) noexcept -> std::expected<zxfoundation::dgp::gate::gate_desc*, lib::kernel_error> {

        const sync::irq_lock_guard _{detail::s_capability_lock};
        scoms::kobject::bobject* ko = nullptr;
        auto result = detail::validate_slot(token, needed, 0, false, ko);
        if (!result) return std::unexpected(result.error());
        if (token.type() != cap_type::gate) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        return reinterpret_cast<zxfoundation::dgp::gate::gate_desc*>(ko);
    }

    template <>
    [[nodiscard]] auto cap_validate<zxfoundation::dgp::portal::portal_desc>(
        cap_token token, ::zxfoundation::dgp::right_mask needed
    ) noexcept -> std::expected<zxfoundation::dgp::portal::portal_desc*, lib::kernel_error> {

        const sync::irq_lock_guard _{detail::s_capability_lock};
        scoms::kobject::bobject* ko = nullptr;
        auto result = detail::validate_slot(token, needed, 0, false, ko);
        if (!result) return std::unexpected(result.error());
        if (token.type() != cap_type::portal) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        return reinterpret_cast<zxfoundation::dgp::portal::portal_desc*>(ko);
    }

    template <>
    [[nodiscard]] auto cap_validate<zxfoundation::memory::objects::memory_object>(
        cap_token token, ::zxfoundation::dgp::right_mask needed
    ) noexcept -> std::expected<zxfoundation::memory::objects::memory_object*, lib::kernel_error> {

        const sync::irq_lock_guard _{detail::s_capability_lock};
        scoms::kobject::bobject* ko = nullptr;
        auto result = detail::validate_slot(token, needed, 0, false, ko);
        if (!result) return std::unexpected(result.error());
        if (token.type() != cap_type::memory) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        auto* object = reinterpret_cast<zxfoundation::memory::objects::memory_object*>(ko);
        if (object->ko.type != zxfoundation::scoms::kobject::kobject_type::grant) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        return object;
    }

} // namespace zxfoundation::dgp::cap
