/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/domain.zcomponent/core.cxx
/// @brief Domain subsystem implementation — domain, strand, and lifecycle
///        management entry points.

module zxfoundation.dgp.domain.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.signal;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.scoms.table;
import zxfoundation.scoms.metadata.core;
import zxfoundation.base.transaction;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.hhdm;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.syschk.core;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.memory.vm;
import zxfoundation.memory.objects;
import zxfoundation.dgp.cap.types;
import zxfoundation.dgp.cap.core;
import zxfoundation.dgp.isolation;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.authorize;
import zxfoundation.dgp.sync_contract;
import zxfoundation.dgp.portal.core;
import zxfoundation.dgp.gate.core;
import zxfoundation.dgp.policy.core;
import zxfoundation.memory.kmalloc;
import zxfoundation.sys.ipc.core;
import zxfoundation.sys.service.core;
import zxfoundation.sched.policy;
import zxfoundation.sched.core;
import arch.s390x.mmu.core;
import arch.s390x.mmu.types;
import arch.s390x.mmu.consts;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.aste;
import lib.error;
import lib.list;
import lib.format;
import lib.cstring;
import lib.container_of;
import std;

namespace zxfoundation::dgp::domain::detail {

    /// @brief The global domain table — SCOMS scoms_table.
    declscope(C) scoms::scoms_table<domain, MAX_DOMAINS> g_domain_table;

    /// @brief Kernel stack size per domain: 4 pages = 16 KiB.
    no_export constexpr u32 DOMAIN_KERNEL_STACK_SIZE  = 4 * 4096;

    /// @brief Buddy order for kernel stack allocation: 2^2 = 4 pages.
    no_export constexpr u32 DOMAIN_KERNEL_STACK_ORDER = 2;

    no_export constexpr u32 MAX_SECONDARY_STRAND_IDS = MAX_DOMAINS * MAX_STRANDS_PER_DOMAIN;

    /// @brief Secondary strand SCOMS table.
    no_export scoms::scoms_table<strand, MAX_SECONDARY_STRAND_IDS> g_strand_table{};

    /// @brief Nucleus domain
    struct nucleus_domain_handle {
        domain* ptr{nullptr};
    };

    no_export nucleus_domain_handle g_nucleus{};

    [[nodiscard]] auto lifecycle_error() noexcept -> lib::kernel_error {
        return lib::kernel_error::from_generic(lib::generic_error::invalid_arg);
    }

    [[nodiscard]] auto claim_domain_destroy(domain& d) noexcept
        -> std::expected<void, lib::kernel_error> {
        const sync::irq_lock_guard guard{d.ko.lock};
        if (d.ko.state != base::lifecycle_state::active
            || d.teardown_phase != dgp::sync_contract::teardown_phase::none
            || !d.ko.transition(base::lifecycle_state::dying)) {
            return std::unexpected(lifecycle_error());
        }
        return {};
    }

    [[nodiscard]] auto advance_domain_teardown_phase(
        domain& d,
        dgp::sync_contract::teardown_phase expected,
        dgp::sync_contract::teardown_phase requested
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const sync::irq_lock_guard guard{d.ko.lock};
        return dgp::sync_contract::advance_teardown_phase(
            d.teardown_phase, expected, requested);
    }

    [[nodiscard]] auto secondary_strand_count(const domain& owner) noexcept -> u32 {
        return static_cast<u32>(owner.strands.count());
    }

    /// @brief Allocate and initialize a secondary strand for a domain.
    /// @param owner  The domain that will own this strand.
    /// @param cpu    CPU affinity (0 for any).
    /// @return Pointer to the created strand, or kernel_error.
    [[nodiscard]] auto strand_create_impl(
        domain* owner, u16 cpu
    ) noexcept -> std::expected<strand*, lib::kernel_error> {
        namespace pmm  = zxfoundation::memory::pmm;
        namespace hhdm = zxfoundation::memory::hhdm;

        if (secondary_strand_count(*owner) >= MAX_STRANDS_PER_DOMAIN) {
            return std::unexpected(
                lib::kernel_error::from_memory(lib::memory_error::out_of_memory));
        }

        if (owner->ko.state != scoms::lifecycle_state::active) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        auto result = g_strand_table.alloc();
        if (!result) return std::unexpected(result.error());

        auto embryo_ref = static_cast<
            scoms::kobject::robject<strand, base::lifecycle_state::embryo>&&
        >(result.value());

        strand& s = *embryo_ref;
        s.ko.generation = owner->ko.generation == 0U ? 1U : owner->ko.generation;

        auto owner_result = scoms::object_core::set_owner(s.ko, owner->ko.id);
        auto attach_result = scoms::object_core::attach(s.ko, owner->ko);
        if (!owner_result || !attach_result) {
            discard_value g_strand_table.release(s.ko.id);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }

        s.owner = owner;
        s.substate = sched::sched_substate::idle;
        s.cpu   = cpu;
        s.on_rq = false;

        // Allocate strand body (cold fields: uregs, strand_list, kstack_base, rt/dl accounting).
        const auto body_result = memory::kmalloc::kmalloc_t<strand_body>();
        if (!body_result) {
            discard_value g_strand_table.release(s.ko.id);
            return std::unexpected(
                lib::kernel_error::from_memory(lib::memory_error::out_of_memory));
        }
        s.body = body_result;

        // Allocate kernel stack.
        auto stack_r = pmm::alloc(
            DOMAIN_KERNEL_STACK_ORDER,
            pmm::zone::dma31,
            pmm::purpose::stack,
            pmm::page_init::zeroed);
        if (!stack_r) {
            memory::kmalloc::kfree_t(s.body);
            discard_value g_strand_table.release(s.ko.id);
            return std::unexpected(stack_r.error());
        }

        const u64 stack_phys = stack_r->phys_base;
        const u64 stack_virt = hhdm::phys_to_virt(stack_phys);
        s.body->kstack_base = stack_virt;
        s.kstack_top  = stack_virt + DOMAIN_KERNEL_STACK_SIZE;
        s.ksp         = s.kstack_top;

        discard_value static_cast<
            scoms::kobject::robject<strand, base::lifecycle_state::embryo>&&
        >(embryo_ref).activate();

        s.substate = sched::sched_substate::runnable;

        // Link into the domain's strand list.
        owner->strands.push_back(s.body->strand_list);

        sys::printk::pverbose("strand: created id={} owner={} cpu={}",
                   s.ko.id, owner->ko.id, static_cast<u32>(cpu));

        return &s;
    }

    [[nodiscard]] auto strand_destroy_impl(strand& s) noexcept -> std::expected<void, lib::kernel_error> {
        namespace pmm  = zxfoundation::memory::pmm;
        namespace hhdm = zxfoundation::memory::hhdm;

        if (s.on_rq || s.substate == sched::sched_substate::running) {
            return std::unexpected(
                lib::kernel_error::from_driver(lib::driver_error::busy));
        }

        if (!s.owner) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        discard_value scoms::kobject::mark_dying(&s);
        lib::list_head::remove(s.body->strand_list);
        discard_value scoms::object_core::detach(s.ko);

        if (s.body->kstack_base != 0U) {
            const u64 stack_phys = hhdm::virt_to_phys(s.body->kstack_base);
            pmm::folio_active stack_folio{
                stack_phys, DOMAIN_KERNEL_STACK_ORDER,
                pmm::purpose::stack, pmm::zone::dma31};
            pmm::free(static_cast<pmm::folio_active&&>(stack_folio));
        }

        memory::kmalloc::kfree_t(s.body);

        discard_value scoms::kobject::mark_destroyed(&s);
        auto release_result = g_strand_table.release(s.ko.id);
        if (!release_result) return std::unexpected(release_result.error());
        return {};
    }

    /// @brief Allocate and initialize a per-CPU idle strand from the SCOMS strand table.
    /// @param cpu  CPU this idle strand is pinned to.
    /// @return Pointer to the created idle strand, or kernel_error.
    /// @note Idle strands have owner == nullptr and strand_flags::idle set.
    [[nodiscard]] auto idle_strand_create_impl(u16 cpu) noexcept
        -> std::expected<strand*, lib::kernel_error> {
        namespace pmm  = zxfoundation::memory::pmm;
        namespace hhdm = zxfoundation::memory::hhdm;

        auto result = g_strand_table.alloc();
        if (!result) return std::unexpected(result.error());

        auto embryo_ref = static_cast<
            scoms::kobject::robject<strand, base::lifecycle_state::embryo>&&
        >(result.value());

        strand& s = *embryo_ref;
        s.ko.generation = 1U;

        s.owner   = g_nucleus.ptr;
        auto owner_result = scoms::object_core::set_owner(
            s.ko, g_nucleus.ptr->ko.id);
        auto attach_result = scoms::object_core::attach(
            s.ko, g_nucleus.ptr->ko);
        if (!owner_result || !attach_result) {
            discard_value g_strand_table.release(s.ko.id);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        s.substate = sched::sched_substate::runnable;
        s.cpu     = cpu;
        s.on_rq   = false;
        s.flags   = strand_flags::idle;
        s.body    = nullptr;

        auto stack_r = pmm::alloc(
            DOMAIN_KERNEL_STACK_ORDER,
            pmm::zone::dma31,
            pmm::purpose::stack,
            pmm::page_init::zeroed);
        if (!stack_r) {
            discard_value g_strand_table.release(s.ko.id);
            return std::unexpected(stack_r.error());
        }

        const u64 stack_phys = stack_r->phys_base;
        const u64 stack_virt = hhdm::phys_to_virt(stack_phys);
        s.kstack_top  = stack_virt + DOMAIN_KERNEL_STACK_SIZE;
        s.ksp         = s.kstack_top;

        discard_value static_cast<
            scoms::kobject::robject<strand, base::lifecycle_state::embryo>&&
        >(embryo_ref).activate();

        return &s;
    }

    /// @brief Destroy a per-CPU idle strand, releasing its stack and SCOMS slot.
    /// @param s Idle strand to destroy (must not be on_rq or running).
    /// @return Success or kernel_error.
    [[nodiscard]] auto idle_strand_destroy_impl(strand& s) noexcept
        -> std::expected<void, lib::kernel_error> {
        namespace pmm  = zxfoundation::memory::pmm;
        namespace hhdm = zxfoundation::memory::hhdm;

        if (s.on_rq || s.substate == sched::sched_substate::running) {
            return std::unexpected(
                lib::kernel_error::from_driver(lib::driver_error::busy));
        }

        discard_value scoms::kobject::mark_dying(&s);

        if (s.kstack_top != 0U) {
            const u64 stack_phys = hhdm::virt_to_phys(s.kstack_top - DOMAIN_KERNEL_STACK_SIZE);
            pmm::folio_active stack_folio{
                stack_phys, DOMAIN_KERNEL_STACK_ORDER,
                pmm::purpose::stack, pmm::zone::dma31};
            pmm::free(static_cast<pmm::folio_active&&>(stack_folio));
        }

        discard_value scoms::kobject::mark_destroyed(&s);
        auto release_result = g_strand_table.release(s.ko.id);
        if (!release_result) return std::unexpected(release_result.error());
        return {};
    }

    /// @brief Transactional domain creation.
    class domain_create_txn : public zxfoundation::base::transaction<domain_create_txn> {
        friend class zxfoundation::base::transaction<domain_create_txn>;

        u32  m_slot{~0U};
        u8   m_skey{0};
        u64  m_root_phys{0};
        u64  m_stack_phys{0};
        zxfoundation::dgp::isolation::isolation_domain_state m_dgp_state{};
        domain* m_raw_domain{nullptr};

        bool m_has_slot{false};
        bool m_has_skey{false};
        bool m_has_root{false};
        bool m_has_stack{false};
        bool m_has_art{false};
        u32  m_address_space_id{scoms::kobject::KOBJECT_ID_INVALID};
        bool m_has_address_space{false};

        /// @brief Rollback handler — releases resources in reverse allocation order.
        auto on_rollback() noexcept -> void {
            namespace pmm  = zxfoundation::memory::pmm;
            namespace mmu  = arch::s390x::mmu;

            if (m_has_stack && m_stack_phys != 0) {
                pmm::folio_active stack_folio{
                    m_stack_phys, DOMAIN_KERNEL_STACK_ORDER,
                    pmm::purpose::stack, pmm::zone::dma31};
                pmm::free(static_cast<pmm::folio_active&&>(stack_folio));
            }

            if (m_has_root && m_root_phys != 0) {
                mmu::dat_table<mmu::dat_level::region_1> root_tbl{m_root_phys};
                mmu::table_free<mmu::dat_level::region_1>(root_tbl);
            }

            if (m_has_art && m_raw_domain) {
                arch::s390x::cpu::aste::art_context_destroy(m_raw_domain->art);
            }

            if (m_has_skey) {
                auto ls = zxfoundation::base::lifecycle_state::sealed;
                discard_value zxfoundation::dgp::isolation::destroy_domain(
                    ls,
                    m_dgp_state.storage_key,
                    m_dgp_state.storage_key_generation,
                    m_dgp_state.storage_key_state,
                    m_dgp_state.revocation_epoch
                );
            }

            if (m_has_address_space) {
                discard_value zxfoundation::memory::objects::memory_object_destroy(
                    m_address_space_id);
            }
            if (m_has_slot) {
                discard_value detail::g_domain_table.release(m_slot);
            }
        }

    public:
        /// @brief Execute the domain creation.
        /// @param[in] name  Human-readable name (must remain valid for
        ///                   the domain's lifetime).
        /// @return Pointer to the activated domain, or kernel_error.
        [[nodiscard]] auto execute(const char* name) noexcept
            -> std::expected<domain*, lib::kernel_error>
        {
            namespace pmm  = zxfoundation::memory::pmm;
            namespace hhdm = zxfoundation::memory::hhdm;
            namespace mmu  = arch::s390x::mmu;
            namespace lc   = arch::s390x::cpu::lowcore;

            auto slot_result = detail::g_domain_table.alloc();
            if (!slot_result)
                return std::unexpected(slot_result.error());

            auto embryo_ref = std::move(*slot_result);
            m_slot     = embryo_ref.id();
            m_has_slot = true;

            zxfoundation::dgp::isolation::isolation_policy policy{};
            policy.required_features = zxfoundation::dgp::isolation::isolation_feature::dat_asce
                                     | zxfoundation::dgp::isolation::isolation_feature::storage_key
                                     | zxfoundation::dgp::isolation::isolation_feature::transactional_vm
                                     | zxfoundation::dgp::isolation::isolation_feature::smp_invalidation;

            auto isolation_result = zxfoundation::dgp::isolation::create_domain(m_slot, policy);
            if (!isolation_result)
                return std::unexpected(isolation_result.error());

            m_dgp_state = *isolation_result;
            m_skey     = m_dgp_state.storage_key;
            m_has_skey = true;

            auto table_result = mmu::crst_alloc<mmu::dat_level::region_1>();
            if (!table_result)
                return std::unexpected(table_result.error());
            auto root_table = *table_result;
            m_root_phys = root_table.origin_phys;
            m_has_root  = true;

            {
                auto* user_r1 = reinterpret_cast<u64*>(hhdm::phys_to_virt(m_root_phys));
                const u64 kernel_asce = lc::get_current()->kernel_asce;
                const u64 kernel_r1_phys = kernel_asce & 0xFFFFFFFFFFFFF000ULL;
                auto* kern_r1 = reinterpret_cast<const u64*>(hhdm::phys_to_virt(kernel_r1_phys));

                std::copy_n(&kern_r1[1024], 1024, &user_r1[1024]);
            }

            auto stack_result = pmm::alloc(
                DOMAIN_KERNEL_STACK_ORDER,
                pmm::zone::dma31,
                pmm::purpose::stack,
                pmm::page_init::zeroed);
            if (!stack_result)
                return std::unexpected(stack_result.error());
            m_stack_phys = stack_result->phys_base;
            m_has_stack  = true;

            {
                domain* d       = embryo_ref.raw();
                lib::string_copy(d->name_storage, std::string_view{name});
                d->name         = d->name_storage;
                d->flags        = domain_flags::none;
                d->storage_key  = m_skey;
                d->root_phys    = m_root_phys;
                d->granted_rights = m_dgp_state.granted_rights;
                d->revocation_epoch.store(m_dgp_state.revocation_epoch, std::memory_order::relaxed);
                d->storage_key_generation = m_dgp_state.storage_key_generation;
                d->storage_key_state = m_dgp_state.storage_key_state;
                d->teardown_phase = zxfoundation::dgp::sync_contract::teardown_phase::none;
                m_raw_domain = d;

                auto art_result = arch::s390x::cpu::aste::art_context_init(d->art);
                if (!art_result) return std::unexpected(art_result.error());
                m_has_art = true;

                auto sealed_ref = std::move(embryo_ref).seal();
                auto asce = mmu::asce_build<mmu::dat_level::region_1>(root_table);
                auto bind_result = zxfoundation::dgp::isolation::bind_asce_root(
                    m_dgp_state, sealed_ref->ko.state, asce, m_root_phys);
                if (!bind_result)
                    return std::unexpected(bind_result.error());

                sealed_ref->asce = asce;

                auto body_result = memory::kmalloc::kmalloc_t<strand_body>();
                if (!body_result) {
                    discard_value detail::g_domain_table.release(m_slot);
                    return std::unexpected(
                        lib::kernel_error::from_memory(lib::memory_error::out_of_memory));
                }
                sealed_ref->primary_strand.body = body_result;
                sealed_ref->primary_strand.body->kstack_base  = hhdm::phys_to_virt(m_stack_phys);
                sealed_ref->primary_strand.kstack_top   = sealed_ref->primary_strand.body->kstack_base + DOMAIN_KERNEL_STACK_SIZE;
                sealed_ref->primary_strand.ksp          = sealed_ref->primary_strand.kstack_top;
                sealed_ref->primary_strand.on_rq        = false;
                sealed_ref->primary_strand.cpu          = 0;
                sealed_ref->primary_strand.owner        = sealed_ref.raw();
                sealed_ref->gate_count   = 0;
                sealed_ref->portal_count = 0;

                ::new(static_cast<void*>(&sealed_ref->strands)) lib::list_head{};
                zxfoundation::memory::vm::vm_space_init(sealed_ref->vm_space);
                sealed_ref->vm_space.owner_domain = sealed_ref->ko.id;
                sealed_ref->vm_space.owner_object = &sealed_ref->ko;
                auto activate_result = zxfoundation::dgp::isolation::activate_domain(
                    sealed_ref->ko.state, sealed_ref->asce, sealed_ref->root_phys, sealed_ref->storage_key);
                if (!activate_result)
                    return std::unexpected(activate_result.error());

                auto active_ref = std::move(sealed_ref).activate();
                if (!active_ref)
                    return std::unexpected(lib::kernel_error::from_generic(
                        lib::generic_error::invalid_arg));

                auto as_auth = zxfoundation::dgp::authorize(
                    *g_nucleus.ptr, *active_ref,
                    zxfoundation::dgp::operation_kind::address_space_create,
                    zxfoundation::dgp::authorization_metadata{
                        .requested_rights = zxfoundation::dgp::isolation::isolation_rights::lifecycle,
                    });
                if (!as_auth) return std::unexpected(as_auth.error());
                auto as_object_result = zxfoundation::memory::objects::memory_object_create(
                    scoms::kobject::kobject_type::address_space,
                    active_ref->ko.id,
                    &active_ref->ko,
                    0,
                    0,
                    zxfoundation::dgp::isolation::isolation_rights::map);
                if (!as_object_result)
                    return std::unexpected(as_object_result.error());
                if (!as_auth->consume()) {
                    discard_value zxfoundation::memory::objects::memory_object_destroy(
                        (*as_object_result)->ko.id);
                    return std::unexpected(lib::kernel_error::from_generic(
                        lib::generic_error::invalid_arg));
                }
                active_ref->vm_space.address_space_object = &(*as_object_result)->ko;
                m_address_space_id = (*as_object_result)->ko.id;
                m_has_address_space = true;
                active_ref->vm_space.storage_key = active_ref->storage_key;
                active_ref->vm_space.storage_key_generation =
                    active_ref->storage_key_generation;
                active_ref->vm_space.numa_node = active_ref->numa_node;
                active_ref->vm_space.asce_val = active_ref->asce.raw();
                m_raw_domain = active_ref.raw();
            }

            commit();
            m_has_address_space = false;
            m_has_art = false;
            return m_raw_domain;
        }
    };

} // namespace zxfoundation::dgp::domain

namespace zxfoundation::dgp::domain {

    using namespace detail;

    /// @brief Initialize the strand SCOMS table.
    auto strand_init() noexcept -> void {
                g_strand_table.init(scoms::kobject::kobject_type::strand);
        sys::printk::pinfo("domain: strand SCOMS table initialized ({} slots)",
            lib::format::dec{static_cast<u64>(MAX_SECONDARY_STRAND_IDS)});
    }

    /// @brief Allocate a per-CPU idle strand from the SCOMS strand table.
    /// @param cpu  CPU this idle strand is pinned to.
    /// @return Pointer to the idle strand, or kernel_error.
    /// @note Idle strands have owner == nullptr and strand_flags::idle set.
    [[nodiscard]] auto idle_strand_create(u16 cpu) noexcept
        -> std::expected<strand*, lib::kernel_error> {
                return idle_strand_create_impl(cpu);
    }

    /// @brief Destroy a per-CPU idle strand and release its SCOMS slot.
    /// @param s Idle strand to destroy.
    /// @return Success or kernel_error.
    [[nodiscard]] auto idle_strand_destroy(strand& s) noexcept
        -> std::expected<void, lib::kernel_error> {
                return idle_strand_destroy_impl(s);
    }

    /// @brief Create a secondary strand within a domain.
    /// @param owner Domain that will own the strand.
    /// @param cpu   CPU affinity (0 for any, or specific CPU).
    /// @return Pointer to the created strand, or kernel_error.
    [[nodiscard]] auto strand_create(
        domain* owner, u16 cpu
    ) noexcept -> std::expected<strand*, lib::kernel_error> {
                if (!owner) return std::unexpected(
            lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        return strand_create_impl(owner, cpu);
    }

    /// @brief Destroy a secondary strand that has already left all run queues.
    /// @param[in,out] s Strand whose stack and SCOMS identity should be released.
    /// @return Success or kernel_error when the strand is still runnable or invalid.
    /// @note The primary embedded strand must be destroyed through `domain_destroy()`.
    /// @warning Callers must dequeue or drain a runnable strand before invoking this function.
    [[nodiscard]] auto strand_destroy(strand& s) noexcept -> std::expected<void, lib::kernel_error> {
                auto* owner = static_cast<domain*>(s.owner);
        if (!owner || &owner->primary_strand == &s) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }
        return strand_destroy_impl(s);
    }

    /// @brief Create a new domain.
    /// @param[in] name  Human-readable name (pointer must remain valid for
    ///                   the domain's lifetime).
    /// @return Pointer to the newly created and activated domain, or kernel_error.
    [[nodiscard]] auto domain_create(const char* name) noexcept
        -> std::expected<domain*, lib::kernel_error>
    {
        
        if (!detail::g_domain_table.initialized())
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::uninitialized));

        if (!name)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        domain_create_txn txn;
        auto result = txn.execute(name);

        if (result) {
            sys::printk::pverbose("domain: created id={} name=\"{}\" skey={}\n",
                               (*result)->ko.id, name, (*result)->storage_key);
        }

        return result;
    }

    /// @brief Destroy a domain and release all its resources.
    /// @param[in] id  Domain ID to destroy.
    /// @return Success or kernel_error on invalid ID or an already-claimed domain.
    [[nodiscard]] auto domain_destroy(domain_id id) noexcept
        -> std::expected<void, lib::kernel_error>
    {
                namespace pmm  = zxfoundation::memory::pmm;
        namespace hhdm = zxfoundation::memory::hhdm;
        namespace mmu  = arch::s390x::mmu;

        if (id >= MAX_DOMAINS)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        if (id == DOMAIN_ID_NUCLEUS)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        auto domain_ref = detail::g_domain_table.acquire_ref<base::lifecycle_state::active>(id);
        if (!domain_ref)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));
        domain* d = domain_ref.raw();

        if ((d->flags & domain_flags::nucleus) != domain_flags::none) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        auto claim_result = claim_domain_destroy(*d);
        if (!claim_result) return std::unexpected(claim_result.error());

        auto teardown_result = advance_domain_teardown_phase(
            *d,
            zxfoundation::dgp::sync_contract::teardown_phase::none,
            zxfoundation::dgp::sync_contract::teardown_phase::deny_new_entries);
        if (!teardown_result) {
            sys::syschk::syschk_fatal(teardown_result.error(),
                "domain: failed to record deny-new-entries phase for {}", id);
        }

        auto revoke_result = zxfoundation::dgp::cap::cap_revoke_for_domain(id);
        if (!revoke_result) {
            sys::syschk::syschk_fatal(revoke_result.error(),
                "domain: capability revoke failed after destroy claim for {}", id);
        }

        auto quiesce_result = sched::quiesce_domain(*d);
        if (!quiesce_result) {
            sys::syschk::syschk_fatal(quiesce_result.error(),
                "domain: quiesce failed after destroy claim for {}", id);
        }

        sys::printk::pverbose("domain: destroying id={} name='{}'\n", id, d->name);

        {
            teardown_result = advance_domain_teardown_phase(
                *d,
                zxfoundation::dgp::sync_contract::teardown_phase::deny_new_entries,
                zxfoundation::dgp::sync_contract::teardown_phase::drain_gates);
            if (!teardown_result) {
                sys::syschk::syschk_fatal(teardown_result.error(),
                    "domain: failed to record gate-drain phase for {}", id);
            }

            gate::destroy_for_domain(id);
        }

        {
            teardown_result = advance_domain_teardown_phase(
                *d,
                zxfoundation::dgp::sync_contract::teardown_phase::drain_gates,
                zxfoundation::dgp::sync_contract::teardown_phase::drain_portals);
            if (!teardown_result) {
                sys::syschk::syschk_fatal(teardown_result.error(),
                    "domain: failed to record portal-drain phase for {}", id);
            }

            portal::destroy_for_domain(id);
        }

        policy::destroy_for_domain(id);
        sys::ipc::destroy_for_domain(id);
        sys::service::destroy_for_domain(id);

        teardown_result = advance_domain_teardown_phase(
            *d,
            zxfoundation::dgp::sync_contract::teardown_phase::drain_portals,
            zxfoundation::dgp::sync_contract::teardown_phase::release_memory);
        if (!teardown_result) {
            sys::syschk::syschk_fatal(teardown_result.error(),
                "domain: failed to record memory-release phase for {}", id);
        }

        zxfoundation::memory::vm::vm_destroy_space(d->vm_space);
        if (d->vm_space.address_space_object) {
            auto as_destroy = zxfoundation::memory::objects::memory_object_destroy(
                d->vm_space.address_space_object->id);
            if (!as_destroy) {
                sys::syschk::syschk_fatal(as_destroy.error(),
                    "domain: address-space release failed after destroy claim for {}", id);
            }
            d->vm_space.address_space_object = nullptr;
        }

        arch::s390x::cpu::aste::art_context_destroy(d->art);

        // Signal storage key revocation to the domain before teardown.
        zxfoundation::dgp::signal::signal_enqueue(
            *d, zxfoundation::dgp::signal::signal_kind::fault, 0xDEADBEEF);

        // Free kernel stack.
        if (d->primary_strand.kstack_top != 0) {
            const u64 stack_phys = hhdm::virt_to_phys(d->primary_strand.kstack_top - DOMAIN_KERNEL_STACK_SIZE);
            pmm::folio_active stack_folio{
                stack_phys, DOMAIN_KERNEL_STACK_ORDER,
                pmm::purpose::stack, pmm::zone::dma31};
            pmm::free(static_cast<pmm::folio_active&&>(stack_folio));
        }
        if (d->primary_strand.body) {
            memory::kmalloc::kfree_t(d->primary_strand.body);
            d->primary_strand.body = nullptr;
        }

        // Free root page table.
        if (d->root_phys != 0) {
            mmu::dat_table<mmu::dat_level::region_1> root_tbl{d->root_phys};
            mmu::table_free<mmu::dat_level::region_1>(root_tbl);
        }

        // Free secondary strands (kernel stacks + slab objects + bodies).
        while (!d->strands.empty()) {
            auto* node = d->strands.pop_front();
            auto* s = lib::container_of<strand_body, lib::list_node, &strand_body::strand_list>(node);
            auto* strand_ptr = reinterpret_cast<strand*>(
                reinterpret_cast<u8*>(s) - __builtin_offsetof(strand, body));
            if (s->kstack_base != 0) {
                const u64 stack_phys = hhdm::virt_to_phys(s->kstack_base);
                pmm::folio_active stack_folio{
                    stack_phys, DOMAIN_KERNEL_STACK_ORDER,
                    pmm::purpose::stack, pmm::zone::dma31};
                pmm::free(static_cast<pmm::folio_active&&>(stack_folio));
            }
            discard_value scoms::object_core::detach(strand_ptr->ko);
            memory::kmalloc::kfree_t(s);
            discard_value g_strand_table.release(strand_ptr->ko.id);
        }

        auto isolation_destroy_result = zxfoundation::dgp::isolation::destroy_domain(
            d->ko.state,
            d->storage_key,
            d->storage_key_generation,
            d->storage_key_state,
            d->revocation_epoch
        );
        if (!isolation_destroy_result) {
            sys::syschk::syschk_fatal(isolation_destroy_result.error(),
                "domain: isolation release failed after destroy claim for {}", id);
        }

        teardown_result = advance_domain_teardown_phase(
            *d,
            zxfoundation::dgp::sync_contract::teardown_phase::release_memory,
            zxfoundation::dgp::sync_contract::teardown_phase::release_identity);
        if (!teardown_result) {
            sys::syschk::syschk_fatal(teardown_result.error(),
                "domain: failed to record identity-release phase for {}", id);
        }

        // table::release transitions dying -> destroyed and retains that terminal
        // state until the final table or external reference is dropped.
        auto release_result = detail::g_domain_table.release(id);
        if (!release_result) {
            sys::syschk::syschk_fatal(release_result.error(),
                "domain: table release failed after destroy claim for {}", id);
        }

        return {};
    }

    /// @brief Look up a domain by ID.
    /// @param[in] id  Domain ID.
    /// @return Pointer to the domain, or nullptr if the slot is unused or
    ///         the ID is out of range.
    [[nodiscard]] auto domain_find(domain_id id) noexcept -> domain* {
        return detail::g_domain_table.find_raw(id);
    }

    /// @brief Activate a domain: transition from embryo → active.
    /// @param[in] id  Domain ID to activate.
    /// @return Success, or kernel_error if the domain is not in embryo state.
    [[nodiscard]] auto domain_activate(domain_id id) noexcept
        -> std::expected<void, lib::kernel_error>
    {
        
        if (id >= MAX_DOMAINS)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        domain* d = detail::g_domain_table.find_raw(id);
        if (!d)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));

        if (!scoms::kobject::swap_state_to(scoms::lifecycle_state::active, d))
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        sys::printk::pinfo("domain: activated id={} name=\"{}\"\n", id, d->name);

        return {};
    }

    /// @brief Return the current number of live domains.
    [[nodiscard]] auto domain_count() noexcept -> u32 {
                return detail::g_domain_table.count();
    }

    auto register_domain_cap_lookup() noexcept -> void {
        discard_value zxfoundation::dgp::cap::register_lookup(
            zxfoundation::dgp::cap::cap_type::domain,
            [](u16 obj_id) noexcept -> scoms::kobject::bobject* {
                auto* d = domain_find(static_cast<domain_id>(obj_id));
                return d ? &d->ko : nullptr;
            });
    }

    /// @brief Initialize the domain subsystem.
    auto domain_init() noexcept -> void {
        
        detail::g_domain_table.init(scoms::kobject::kobject_type::domain);

        register_domain_cap_lookup();

        sys::printk::pinfo("domain: subsystem initialized, max_domains={}\n",
                           MAX_DOMAINS);
    }

    /// @brief Get the nucleus domain — the kernel's own domain identity.
    /// @return Pointer to the nucleus domain (slot 0).  Always non-null
    ///         after @ref domain_init_nucleus has been called.
    [[nodiscard]] auto domain_nucleus() noexcept -> domain* {
        return g_nucleus.ptr;
    }

    /// @brief Return the currently-running domain.
    [[nodiscard]] auto current_domain_or_nucleus() noexcept -> domain* {
        auto* current = static_cast<domain*>(
            arch::s390x::cpu::lowcore::current_domain_ptr());
        if (current != nullptr) [[likely]] return current;
        return domain_nucleus();
    }

    /// @brief Initialize the nucleus domain at slot 0.
    [[nodiscard]] auto domain_init_nucleus() noexcept
        -> std::expected<void, lib::kernel_error> {
                namespace mmu = arch::s390x::mmu;

        if (!detail::g_domain_table.initialized())
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::uninitialized));

        auto slot_r = detail::g_domain_table.alloc_at(0);
        if (!slot_r) {
            sys::printk::pcrit("domain: nucleus slot 0 already occupied — "
                              "boot-order violation\n");
            return std::unexpected(slot_r.error());
        }

        auto embryo_ref = static_cast<
            scoms::kobject::robject<domain, base::lifecycle_state::embryo>&&
        >(slot_r.value());

        domain* d = embryo_ref.raw();
        d->ko.generation = 1U;

        lib::string_copy(d->name_storage, std::string_view{"foundation/nucleus"});
        d->name = d->name_storage;
        d->flags = domain_flags::nucleus;

        d->storage_key              = SKEY_NUCLEUS;
        d->storage_key_state        = zxfoundation::dgp::isolation::skey_revocation_state::active;
        d->storage_key_generation   = 1U;
        d->revocation_epoch.store(0U, std::memory_order::relaxed);
        d->teardown_phase            = zxfoundation::dgp::sync_contract::teardown_phase::none;
        d->numa_node                = 0U;
        d->granted_rights           = zxfoundation::dgp::isolation::isolation_rights::all;

        u64 cr1{0};
        arch::s390x::cpu::ctlreg::ctl_store<1U, 1U>(cr1);
        if (cr1 == 0U) {
            sys::printk::pcrit("domain: kernel CR1 is zero at nucleus init\n");
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        d->asce = arch::s390x::mmu::typed_asce<arch::s390x::mmu::dat_level::region_1>{cr1};
        d->root_phys = cr1 & mmu::ASCE_ORIGIN_MASK;
        d->vm_space.asce_val = d->asce.raw();
        auto art_result = arch::s390x::cpu::aste::art_context_init(d->art);
        if (!art_result) return std::unexpected(art_result.error());

        d->primary_strand.kstack_top = 0U;
        d->primary_strand.ksp        = 0U;
        d->primary_strand.on_rq      = false;
        d->primary_strand.cpu        = 0U;
        d->primary_strand.substate   = sched::sched_substate::idle;
        d->primary_strand.owner      = d;
        d->primary_strand.body       = nullptr;
        d->primary_strand.flags      = strand_flags::idle;

        if (!scoms::kobject::swap_state_to(zxfoundation::base::lifecycle_state::active, d)) {
            sys::printk::pcrit("domain: nucleus state swap failed\n");
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::internal_error));
        }

        g_nucleus.ptr = d;

        {
            auto* lc = arch::s390x::cpu::lowcore::get_current();
            if (lc) [[likely]] {
                lc->current_domain = reinterpret_cast<u64>(d);
            }
        }

        return {};
    }

} // namespace zxfoundation::dgp::domain

