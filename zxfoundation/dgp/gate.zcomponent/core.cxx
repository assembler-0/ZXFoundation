/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/gate.zcomponent/core.cxx
/// @brief Gate subsystem core — gate lifecycle and the SCOMS-managed gate
///        table.

module zxfoundation.dgp.gate.core;
import zxfoundation.dgp.gate.types;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.dgp.cap.core;
import zxfoundation.dgp.cap.types;
import zxfoundation.dgp.authorize;
import zxfoundation.dgp.domain.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.gate.registration;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.scoms.metadata.core;
import zxfoundation.scoms.table;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.zinit.core;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.pcss.core;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.aste;
import lib.error;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

using zxfoundation::dgp::gate::gate_desc;
using zxfoundation::dgp::gate::gate_type;
using zxfoundation::dgp::gate::gate_auth;
using zxfoundation::dgp::gate::MAX_GATES;
using zxfoundation::sys::printk::pinfo;
using zxfoundation::sys::printk::perr;

namespace zxfoundation::dgp::gate::detail {

    using namespace zxfoundation::scoms;
    using zxfoundation::base::lifecycle_state;

    /// @brief SCOMS-managed gate table.
    static scoms_table<gate_desc, MAX_GATES> g_gate_table{};

} // namespace detail

namespace zxfoundation::dgp::gate {

    /// @brief Create a new gate descriptor.
    /// @param type             Gate type (nucleus or domain).
    /// @param auth             Authorization policy.
    /// @param target_domain_id Target domain ID.
    /// @param target_key       Storage key of the target domain.
    /// @return                 The allocated slot ID, or an error if the table is full.
    auto gate_create(
        gate_type type,
        gate_auth auth,
        u32       target_domain_id,
        u8        target_key,
        u64       entry_va
    ) noexcept -> std::expected<u32, lib::kernel_error> {

        auto* caller = []() -> zxfoundation::dgp::domain::domain* {
            auto* lc = arch::s390x::cpu::lowcore::get_current();
            if (lc && lc->get<"current_domain">() != 0)
                return reinterpret_cast<zxfoundation::dgp::domain::domain*>(lc->get<"current_domain">());
            return nullptr;
        }();
        auto* target = zxfoundation::dgp::domain::domain_find(target_domain_id);
        if (!caller || !target) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        auto auth_result = zxfoundation::dgp::authorize(
            *caller, *target,
            zxfoundation::dgp::operation_kind::gate_create,
            zxfoundation::dgp::authorization_metadata{
                .requested_rights = ( ::zxfoundation::dgp::right::create | ::zxfoundation::dgp::right::destroy ),
            }
        );
        if (!auth_result) {
            return std::unexpected(auth_result.error());
        }

        if (target->storage_key != target_key) {
            perr("gate_create: target_key={} does not match target domain {} storage_key={}",
                 target_key, target_domain_id, target->storage_key);
            return std::unexpected(lib::kernel_error::from_memory(lib::memory_error::capability_invalid));
        }

        auto alloc_result = detail::g_gate_table.alloc();
        if (!alloc_result) {
            perr("gate: gate table full ({} / {})",
                 detail::g_gate_table.count(), MAX_GATES);
            return std::unexpected(alloc_result.error());
        }

        auto embryo_ref = static_cast<
            scoms::kobject::robject<gate_desc, base::lifecycle_state::embryo>&&
        >(alloc_result.value());

        gate_desc& desc   = *embryo_ref;
        auto owner_result = scoms::object_core::set_owner(desc.ko, target->ko.id);
        auto attach_result = scoms::object_core::attach(desc.ko, target->ko);
        if (!owner_result || !attach_result) {
            discard_value detail::g_gate_table.release(desc.ko.id);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }

        arch::s390x::cpu::pcss::gate_request req{};
        req.asn = static_cast<u16>(target->pc_asn);
        req.entry_va = entry_va;
        req.extended_addressing = true;
        req.ekm = 0xFFFFU;
        req.entry_key_mask = 0xFFFFU;
        req.entry_key = static_cast<u8>(target->storage_key & 0x0FU);
        req.problem_state = true;
        arch::s390x::cpu::pcss::pc_number nr{};
        auto mat = arch::s390x::cpu::pcss::gate_materialize(
            target->pc_tables, caller->pc_tables, req, nr);
        if (!mat) return std::unexpected(mat.error());

        desc.type          = type;
        desc.auth          = auth;
        desc.target_domain = target_domain_id;
        desc.target_key    = target_key;
        desc.et_index      = nr.get<"ex">();
        desc.class_mask    = req.ekm;
        desc.pc_nr         = nr;

        const u32 id = desc.ko.id;

        auto active_ref = static_cast<
            scoms::kobject::robject<gate_desc, base::lifecycle_state::embryo>&&
        >(embryo_ref).activate();

        ++target->gate_count;

        sys::printk::pverbose("gate: created gate {} (type={}, target_domain={}, entry={:#x})",
              id,
              static_cast<u8>(type),
              target_domain_id, entry_va);

        return id;
    }

    /// @brief Destroy a gate, freeing its slot.
    /// @param id  The gate slot ID to destroy.
    /// @return    Success, or an error if the id is invalid or already free.
    auto gate_destroy(u32 id) noexcept
        -> std::expected<void, lib::kernel_error> {

        auto* gate = detail::g_gate_table.find_raw(id);
        if (!gate) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));
        }
        const u32 owner_id = gate->target_domain;
        auto revoke_result = zxfoundation::dgp::cap::cap_revoke_target(gate->ko);
        if (!revoke_result) return std::unexpected(revoke_result.error());

        auto result = detail::g_gate_table.release(id);
        if (!result) {
            perr("gate: gate_destroy — id {} failed", id);
            return std::unexpected(result.error());
        }

        if (auto* owner = zxfoundation::dgp::domain::domain_find(owner_id);
            owner != nullptr && owner->gate_count > 0U) {
            --owner->gate_count;
        }

        sys::printk::pverbose("gate: destroyed gate {}", id);
        return {};
    }

    /// @brief Destroy all gates owned by a specific domain.
    /// @param domain_id  The domain whose gates should be destroyed.
    auto destroy_for_domain(u32 domain_id) noexcept -> void {
        detail::g_gate_table.destroy_if(
            [domain_id](const gate_desc& g) noexcept -> bool {
                return g.target_domain == domain_id;
            });
    }

    /// @brief Look up a gate descriptor by id.
    /// @param id  The gate slot ID to look up.
    /// @return    Pointer to the gate_desc, or nullptr if the id is invalid
    ///            or the slot is unused.
    auto gate_find(u32 id) noexcept -> gate_desc* {
        return detail::g_gate_table.find_raw(id);
    }

    auto register_gate_cap_lookup() noexcept -> void {
        discard_value zxfoundation::dgp::cap::register_lookup(
            zxfoundation::dgp::cap::cap_type::gate,
            [](u16 obj_id) noexcept -> scoms::kobject::bobject* {
                auto* g = gate_find(static_cast<u32>(obj_id));
                return g ? &g->ko : nullptr;
            });
    }

    /// @brief Initialize the gate subsystem.
    auto zi_gate_init(const sys::zinit::zi_desc&) noexcept -> sys::zinit::zi_result {
        detail::g_gate_table.init(scoms::kobject::kobject_type::gate);
        register_gate_cap_lookup();
        registration::register_all_handlers();
        return {};
    }
    __zinit__(dgp, p40, zi_gate_init, fatal)

    /// @brief Return the current number of active gates.
    auto gate_count() noexcept -> u32 {
        return detail::g_gate_table.count();
    }

} // namespace zxfoundation::dgp::gate