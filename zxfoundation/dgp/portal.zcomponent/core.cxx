/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/portal.zcomponent/core.cxx
/// @brief Portal lifecycle

module zxfoundation.dgp.portal.core;
import zxfoundation.dgp.portal.types;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.domain.core;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.scoms.table;
import zxfoundation.scoms.metadata.core;
import zxfoundation.base.transaction;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.syschk.core;
import zxfoundation.dgp.cap.types;
import zxfoundation.dgp.cap.core;
import zxfoundation.dgp.isolation;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.authorize;
import zxfoundation.dgp.sync_contract;
import zxfoundation.memory.vm;
import zxfoundation.memory.vm.fault;
import zxfoundation.memory.vm.types;
import zxfoundation.memory.pmm;
import zxfoundation.memory.hhdm;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.base.typestate;
import zxfoundation.smp.csd;
import zxfoundation.sys.zinit.core;
import arch.s390x.cpu.ar;
import arch.s390x.cpu.aste;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.sync;
import arch.s390x.mmu;
import lib.error;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

using zxfoundation::dgp::portal::portal_desc;
using zxfoundation::dgp::portal::portal_prot;
using zxfoundation::dgp::portal::MAX_PORTALS;

namespace zxfoundation::dgp::portal::detail {

    using namespace zxfoundation::scoms;
    using zxfoundation::base::lifecycle_state;

    /// @brief SCOMS-managed portal table (replaces ad-hoc array + lock + count).
    no_export scoms_table<portal_desc, MAX_PORTALS> g_portal_table{};
    alignas(4) no_export zxfoundation::sync::qspinlock::qspinlock<zxfoundation::sync::lock_rank::portal_ar> g_portal_lock{};
    no_export std::atomic_flag g_alb_purge_lock{};
    no_export ::lib::cpumask g_alb_purge_mask{};
    no_export std::array<smp::csd::call_single_data, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS>
        g_alb_purge_csds{};

    auto portal_root_destroy(portal_desc& desc) noexcept -> void;

    auto purge_alb_on_cpu(u64, void*) noexcept -> void {
        arch::s390x::cpu::ar::purge_alb();
    }

    [[nodiscard]] auto purge_alb_all_cpus() noexcept
        -> std::expected<void, lib::kernel_error> {
        while (g_alb_purge_lock.test_and_set(std::memory_order::acquire))
            arch::s390x::cpu::sync::cpu_relax();

        arch::s390x::cpu::ar::purge_alb();
        g_alb_purge_mask.reset();
        arch::s390x::cpu::cpumask::for_each_online_cpu(
            [](u16 cpu) noexcept -> void { g_alb_purge_mask.set(cpu); });
        auto result = zxfoundation::smp::csd::call_function_many(
            g_alb_purge_mask,
            purge_alb_on_cpu, nullptr, 0U,
            g_alb_purge_csds.data(),
            zxfoundation::base::config::CONFIG_ZX_MAX_CPUS);

        g_alb_purge_lock.clear(std::memory_order::release);
        if (!result) return std::unexpected(result.error());
        return {};
    }

    [[nodiscard]] auto portal_root_create(
        portal_desc& desc, zxfoundation::memory::vm::vm_region& source,
        zxfoundation::dgp::portal::portal_prot prot
    ) noexcept -> std::expected<void, lib::kernel_error> {
        namespace mmu = arch::s390x::mmu;
        namespace pmm = zxfoundation::memory::pmm;
        auto root_result = mmu::crst_alloc<mmu::dat_level::region_1>();
        if (!root_result) return std::unexpected(root_result.error());
        const auto root = *root_result;
        desc.portal_root_phys = root.origin_phys;
        desc.portal_asce = mmu::asce_build(root).raw();

        const bool writable = (prot & zxfoundation::dgp::portal::portal_prot::write)
            != zxfoundation::dgp::portal::portal_prot::none;
        if (writable) {
            mmu::table_free(root);
            desc.portal_root_phys = 0;
            desc.portal_asce = 0;
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }

        const mmu::page_prot page_prot{mmu::PTE_PROTECT | mmu::PTE_NOEXEC};
        for (u64 source_va = source.va_start; source_va < source.va_end;
             source_va += PAGE_SIZE) {
            auto* slot = source.find_page(source_va);
            if (!slot || slot->phys == 0U) {
                portal_root_destroy(desc);
                desc.portal_root_phys = 0;
                desc.portal_asce = 0;
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::invalid_address));
            }
            auto pin = pmm::frame_ref(slot->phys);
            if (!pin) {
                portal_root_destroy(desc);
                return std::unexpected(pin.error());
            }
            const u64 portal_va = desc.va_start + (source_va - source.va_start);
            auto map = mmu::map_page(root, portal_va, slot->phys, page_prot);
            if (!map) {
                discard_value pmm::frame_release_pin(slot->phys);
                portal_root_destroy(desc);
                return std::unexpected(map.error());
            }
            ++desc.pinned_pages;
        }
        return {};
    }

    auto portal_root_destroy(portal_desc& desc) noexcept -> void {
        if (desc.portal_root_phys == 0U) return;
        namespace mmu = arch::s390x::mmu;
        namespace pmm = zxfoundation::memory::pmm;
        const mmu::dat_table<mmu::dat_level::region_1> root{desc.portal_root_phys};
        for (u64 va = desc.va_start; va < desc.va_end; va += PAGE_SIZE) {
            auto phys = mmu::unmap_page(root, va);
            if (phys) discard_value pmm::frame_release_pin(*phys);
        }
        mmu::tlb_flush_asce(desc.portal_asce);
        mmu::table_free(root);
        desc.portal_root_phys = 0;
        desc.portal_asce = 0;
        desc.pinned_pages = 0;
    }

    [[nodiscard]] auto allocate_ar(zxfoundation::dgp::domain::domain& source) noexcept
        -> std::expected<u8, lib::kernel_error> {
        const zxfoundation::sync::irq_lock_guard _{g_portal_lock};
        for (u8 ar = 1U; ar < arch::s390x::cpu::ar::AR_COUNT; ++ar) {
            const u16 bit = static_cast<u16>(1U << ar);
            if ((source.portal_ar_mask & bit) == 0U) {
                source.portal_ar_mask |= bit;
                return ar;
            }
        }
        return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::out_of_memory));
    }

    auto release_ar(zxfoundation::dgp::domain::domain& source, u8 ar) noexcept -> void {
        const zxfoundation::sync::irq_lock_guard _{g_portal_lock};
        if (ar < arch::s390x::cpu::ar::AR_COUNT)
            source.portal_ar_mask &= static_cast<u16>(~(1U << ar));
    }

    /// @brief CRTP transaction that tracks the multi-step portal creation.
    class portal_create_txn : public zxfoundation::base::transaction<portal_create_txn> {
        friend class zxfoundation::base::transaction<portal_create_txn>;

        u32  m_slot_id{~0U};
        bool m_slot_allocated{false};

        u32  m_aste_index{~0U};
        bool m_aste_allocated{false};
        portal_desc* m_desc{nullptr};

        u32  m_alet{0};
        bool m_ale_created{false};
        bool m_ar_allocated{false};
        zxfoundation::dgp::domain::domain* m_source{nullptr};

        /// @brief Rollback handler — called by ~transaction() if not committed.
        auto on_rollback() noexcept -> void {
            // Reverse order teardown.
            if (m_ale_created) {
                if (m_desc) {
                    if (auto* src = zxfoundation::dgp::domain::domain_find(m_desc->source_domain)) {
                        discard_value arch::s390x::cpu::aste::ale_destroy(src->art, m_alet);
                    }
                }
            }
            if (m_ar_allocated && m_source && m_desc)
                detail::release_ar(*m_source, m_desc->ar_num);
            if (m_aste_allocated) {
                discard_value arch::s390x::cpu::aste::aste_free(m_aste_index);
            }
            if (m_desc) detail::portal_root_destroy(*m_desc);
            if (m_slot_allocated) {
                discard_value detail::g_portal_table.release(m_slot_id);
            }
        }

    public:
        portal_create_txn() noexcept = default;

        auto record_slot(u32 id) noexcept -> void {
            m_slot_id        = id;
            m_slot_allocated = true;
        }

        auto record_aste(u32 idx) noexcept -> void {
            m_aste_index    = idx;
            m_aste_allocated = true;
        }

        auto record_desc(portal_desc& desc) noexcept -> void { m_desc = &desc; }

        auto record_ar(zxfoundation::dgp::domain::domain& source) noexcept -> void {
            m_ar_allocated = true;
            m_source = &source;
        }

        auto record_ale(u32 alet) noexcept -> void {
            m_alet       = alet;
            m_ale_created = true;
        }
    };

} // namespace detail

namespace zxfoundation::dgp::portal {

    /// @brief Create a new portal from @p source_domain_id to @p target_domain_id.
    /// @param source_domain_id  Domain that will hold the portal (owns the AR).
    /// @param target_domain_id  Domain whose address space is accessed.
    /// @param va_start          Start of the accessible VA range in the target.
    /// @param va_end            End (exclusive) of the accessible VA range.
    /// @param prot              Access protection (read, write, rw).
    /// @param ar_num            Access register number (0-15) in the source domain.
    ///
    /// @return The portal slot ID of the newly created portal, or a kernel_error.
    [[nodiscard]] auto portal_create(
        u32         source_domain_id,
        u32         target_domain_id,
        u64         va_start,
        u64         va_end,
        portal_prot prot,
        u8          ar_num,
        zxfoundation::memory::vm::vm_region* backing
    ) noexcept -> std::expected<u32, lib::kernel_error> {

        if (ar_num >= arch::s390x::cpu::ar::AR_COUNT) {
            return std::unexpected(
                lib::kernel_error::from_generic(
                    lib::generic_error::invalid_arg));
        }

        if (va_end <= va_start) {
            return std::unexpected(
                lib::kernel_error::from_generic(
                    lib::generic_error::invalid_arg));
        }
        if ((va_start & (PAGE_SIZE - 1U)) != 0U ||
            (va_end & (PAGE_SIZE - 1U)) != 0U) {
            return std::unexpected(
                lib::kernel_error::from_memory(lib::memory_error::alignment_error));
        }

        auto* src = zxfoundation::dgp::domain::domain_find(source_domain_id);
        if (!src) {
            return std::unexpected(
                lib::kernel_error::from_generic(
                    lib::generic_error::not_found));
        }
        auto* tgt = zxfoundation::dgp::domain::domain_find(target_domain_id);
        if (!tgt) {
            return std::unexpected(
                lib::kernel_error::from_generic(
                    lib::generic_error::not_found));
        }
        auto auth_result = zxfoundation::dgp::authorize(
            *src, *tgt,
            zxfoundation::dgp::operation_kind::portal_create,
            zxfoundation::dgp::authorization_metadata{
                .requested_rights = ::zxfoundation::dgp::right::share | ::zxfoundation::dgp::right::map,
                .target_va_start = va_start,
                .target_va_end = va_end,
            }
        );
        if (!auth_result) {
            return std::unexpected(auth_result.error());
        }

        detail::portal_create_txn txn;

        if (ar_num == 0U) {
            auto ar_result = detail::allocate_ar(*src);
            if (!ar_result) return std::unexpected(ar_result.error());
            ar_num = *ar_result;
        } else {
            const auto ar_result = detail::allocate_ar(*src);
            if (!ar_result) return std::unexpected(ar_result.error());
            if (*ar_result != ar_num) {
                detail::release_ar(*src, *ar_result);
                return std::unexpected(lib::kernel_error::from_driver(
                    lib::driver_error::busy));
            }
        }
        txn.record_ar(*src);

        auto alloc_result = detail::g_portal_table.alloc();
        if (!alloc_result) {
            detail::release_ar(*src, ar_num);
            return std::unexpected(alloc_result.error());
        }

        auto embryo_ref = static_cast<
            scoms::kobject::robject<portal_desc, base::lifecycle_state::embryo>&&
        >(alloc_result.value());

        txn.record_slot(embryo_ref.id());

        // Ensure the target domain still exists (may have been destroyed while we were blocked).
        tgt = zxfoundation::dgp::domain::domain_find(target_domain_id);
        if (!tgt) {
            // txn destructor will release the slot.
            return std::unexpected(
                lib::kernel_error::from_generic(
                    lib::generic_error::not_found));
        }

        portal_desc& desc = *embryo_ref;
        txn.record_desc(desc);
        auto owner_result = scoms::object_core::set_owner(desc.ko, src->ko.id);
        auto attach_result = scoms::object_core::attach(desc.ko, src->ko);
        if (!owner_result || !attach_result) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        desc.source_domain = source_domain_id;
        desc.target_domain = target_domain_id;
        desc.va_start      = va_start;
        desc.va_end        = va_end;
        desc.prot          = prot;
        desc.ar_num        = ar_num;

        zxfoundation::memory::vm::vm_region_ref source_region_ref{};
        auto* source_region = backing;
        if (!source_region) {
            source_region_ref = zxfoundation::memory::vm::vm_find_ref(
                tgt->vm_space, va_start);
            source_region = source_region_ref.raw();
        }
        if (!source_region ||
            (!backing && source_region->va_start != va_start) ||
            source_region->size() != va_end - va_start ||
            source_region->owner_domain != tgt->ko.id) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        desc.backing_va_start = source_region->va_start;
        auto root_result = detail::portal_root_create(desc, *source_region, prot);
        if (!root_result) return std::unexpected(root_result.error());

        auto aste_result = arch::s390x::cpu::aste::aste_alloc(desc.portal_asce, 0);
        if (!aste_result) return std::unexpected(aste_result.error());
        const u32 aste_index = *aste_result;
        desc.aste_index = aste_index;
        txn.record_aste(aste_index);

        auto ale_result = arch::s390x::cpu::aste::ale_create(src->art, aste_index);
        if (!ale_result) return std::unexpected(ale_result.error());
        const u32 alet = *ale_result;
        desc.alet = alet;
        txn.record_ale(alet);

        const u32 id = desc.ko.id;

        // Transition embryo → active.
        auto active_ref = static_cast<
            scoms::kobject::robject<portal_desc, base::lifecycle_state::embryo>&&
        >(embryo_ref).activate();

        // Increment source domain's portal_count.
        if (src) {
            ++src->portal_count;
        }

        // Commit the transaction — disarms the automatic rollback.
        txn.commit();

        sys::printk::pverbose("portal: created id={} src={} tgt={} ar={} alet={} "
                   "va=[{:#x},{:#x})",
                   id, source_domain_id, target_domain_id,
                   static_cast<u32>(ar_num), alet, va_start, va_end);

        return id;
    }

    [[nodiscard]] auto resolve_fault(
        u32 accessor_domain, u8 ar_num, u32 alet,
        u64 fault_va, bool is_write
    ) noexcept -> std::pair<fault_outcome, u32> {
        namespace mmu = arch::s390x::mmu;
        namespace pmm = zxfoundation::memory::pmm;
        namespace vm  = zxfoundation::memory::vm;

        portal_desc* live = nullptr;
        for (u32 id = 0U; id < MAX_PORTALS; ++id) {
            auto* cand = detail::g_portal_table.find_raw(id);
            if (!cand || cand->ko.state != zxfoundation::base::lifecycle_state::active)
                continue;
            if (cand->source_domain == accessor_domain && cand->ar_num == ar_num) {
                live = cand;
                break;
            }
        }

        if (live == nullptr) return {fault_outcome::stale, 0U};
        if (live->alet != alet) return {fault_outcome::stale, live->ko.id};
        if (fault_va < live->va_start || fault_va >= live->va_end)
            return {fault_outcome::stale, live->ko.id};
        if (is_write && (live->prot & portal_prot::write) == portal_prot::none)
            return {fault_outcome::stale, live->ko.id};

        const u64 backing_va = live->backing_va_start
            + (fault_va - live->va_start);
        auto* target = zxfoundation::dgp::domain::domain_find(live->target_domain);
        if (target != nullptr) {
            const auto action = vm::handle_page_fault(
                target->vm_space, target->root_phys, target->storage_key,
                backing_va, is_write, false);
            if (action == vm::fault_action::resolved ||
                action == vm::fault_action::stack_grew) {
                auto region_ref = vm::vm_find_ref(target->vm_space, backing_va);
                auto* region = region_ref.raw();
                if (region != nullptr) {
                    if (auto* slot = region->find_page(backing_va); slot && slot->phys != 0U) {
                        auto pin = pmm::frame_ref(slot->phys);
                        if (pin) {
                            mmu::dat_table<mmu::dat_level::region_1> root{
                                live->portal_root_phys};
                            const mmu::page_prot prot{
                                mmu::PTE_PROTECT | mmu::PTE_NOEXEC};
                            if (mmu::map_page(root, fault_va, slot->phys, prot)) {
                                sys::printk::pverbose(
                                    "portal: fault resolved id={} va={:#x}",
                                    live->ko.id, fault_va);
                                return {fault_outcome::resolved, live->ko.id};
                            }
                            discard_value pmm::frame_release_pin(slot->phys);
                        }
                    }
                }
            }
        }

        return {fault_outcome::revoked, live->ko.id};
    }

    [[nodiscard]] auto portal_find(u32 id) noexcept -> portal_desc* {
        return detail::g_portal_table.find_raw(id);
    }

    /// @brief Destroy the portal identified by @p id.
    /// @param id  Portal slot ID returned by portal_create().
    /// @return    Nothing on success, or a kernel_error.
    [[nodiscard]] auto portal_destroy(u32 id) noexcept
        -> std::expected<void, lib::kernel_error> {

        // Look up the portal before releasing.
        auto* desc = detail::g_portal_table.find_raw(id);
        if (!desc) {
            return std::unexpected(
                lib::kernel_error::from_generic(
                    lib::generic_error::not_found));
        }

        auto revoke_result = zxfoundation::dgp::cap::cap_revoke_target(desc->ko);
        if (!revoke_result) return std::unexpected(revoke_result.error());

        sys::printk::pverbose("portal: destroying id={} src={} tgt={} alet={}\n",
                   desc->ko.id, desc->source_domain, desc->target_domain, desc->alet);

        // Convert the DGP portal authority into a draining revocation before hardware reuse.
        auto* src = zxfoundation::dgp::domain::domain_find(desc->source_domain);
        if (src) {
            auto revoke_result = zxfoundation::dgp::isolation::begin_portal_teardown(
                src->ko.state,
                src->revocation_epoch);
            if (!revoke_result) {
                return std::unexpected(revoke_result.error());
            }
            if (src->portal_count > 0) --src->portal_count;
        }

        if (src && desc->alet > arch::s390x::cpu::aste::ALET_RESERVED_MAX) {
            discard_value arch::s390x::cpu::aste::ale_destroy(src->art, desc->alet);
            detail::release_ar(*src, desc->ar_num);
        }
        auto purge_result = detail::purge_alb_all_cpus();
        if (!purge_result) {
            zxfoundation::sys::syschk::syschk_fatal(
                purge_result.error(), "portal: ALB purge failed during destruction");
        }
        detail::portal_root_destroy(*desc);
        if (desc->aste_index != ~0U)
            discard_value arch::s390x::cpu::aste::aste_free(desc->aste_index);

        // Release the slot back to the SCOMS table.
        auto result = detail::g_portal_table.release(id);
        if (!result) {
            return std::unexpected(result.error());
        }

        return {};
    }

    /// @brief Destroy all portals involving a specific domain.
    /// @param domain_id  The domain whose portals should be destroyed.
    auto destroy_for_domain(u32 domain_id) noexcept -> void {
        detail::g_portal_table.destroy_if(
            // Predicate: match portals where domain is source OR target.
            [domain_id](const portal_desc& p) noexcept -> bool {
                return p.source_domain == domain_id ||
                       p.target_domain == domain_id;
            },
            // Cleanup: revoke DGP authority, purge ALB, then tear down hardware before the slot is released.
            [](portal_desc& p) noexcept -> void {
                auto* src = zxfoundation::dgp::domain::domain_find(p.source_domain);
                if (src) {
                    discard_value zxfoundation::dgp::isolation::begin_portal_teardown(
                        src->ko.state,
                        src->revocation_epoch);
                    if (src->portal_count > 0) --src->portal_count;
                }

                if (src && p.alet > arch::s390x::cpu::aste::ALET_RESERVED_MAX) {
                    discard_value arch::s390x::cpu::aste::ale_destroy(src->art, p.alet);
                    detail::release_ar(*src, p.ar_num);
                }
                auto purge_result = detail::purge_alb_all_cpus();
                if (!purge_result) {
                    zxfoundation::sys::syschk::syschk_fatal(
                        purge_result.error(),
                        "portal: ALB purge failed during domain teardown");
                }
                detail::portal_root_destroy(p);
                if (p.aste_index != ~0U)
                    discard_value arch::s390x::cpu::aste::aste_free(p.aste_index);
            });
    }

    auto register_portal_cap_lookup() noexcept -> void {
        discard_value zxfoundation::dgp::cap::register_lookup(
            zxfoundation::dgp::cap::cap_type::portal,
            [](u16 obj_id) noexcept -> scoms::kobject::bobject* {
                auto* p = portal_find(static_cast<u32>(obj_id));
                return p ? &p->ko : nullptr;
            });
    }

    /// @brief Initialize the portal subsystem.
    auto zi_portal_init(const sys::zinit::zi_desc&) noexcept -> sys::zinit::zi_result {
        detail::g_portal_table.init(scoms::kobject::kobject_type::portal);
        register_portal_cap_lookup();
        return {};
    }
    __zinit_dep__(dgp, p60, zi_portal_init, fatal, "zi_gate_register_handlers")


    /// @brief Return the number of currently active portals in the system.
    [[nodiscard]] auto portal_count() noexcept -> u32 {
        return detail::g_portal_table.count();
    }

} // namespace zxfoundation::dgp::portal
