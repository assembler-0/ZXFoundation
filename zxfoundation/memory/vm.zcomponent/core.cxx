/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/memory/vm.zcomponent/core.cxx
/// @brief Virtual Memory Engine — core operations.

module zxfoundation.memory.vm.core;
import zxfoundation.memory.vm.types;
import zxfoundation.dgp.memory_contract;
import zxfoundation.dgp.isolation.types;
import zxfoundation.sync.rcu;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.table;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.scoms.metadata.core;
import zxfoundation.memory.objects;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.kmalloc;
import zxfoundation.memory.numa.types;
import zxfoundation.memory.hhdm;
import zxfoundation.memory.page_cache;
import zxfoundation.memory.page_cache.types;
import zxfoundation.memory.rmap;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.printk.types;
import zxfoundation.sys.syschk.core;
import zxfoundation.base.typestate;
import zxfoundation.base.types;
import zxfoundation.dgp.cap.types;
import zxfoundation.dgp.cap.core;
import arch.s390x.mmu;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.skey;
import lib.error;
import lib.rbtree;
import lib.format;
import lib.container_of;
import lib.stl;

namespace zxfoundation::memory::vm::detail {

    using namespace zxfoundation::memory;

    no_export scoms::scoms_table<vm_region, MAX_VM_REGIONS> g_region_table{};
    no_export bool g_vme_initialized{false};

    auto destroy_region_memory_objects(vm_region& rgn) noexcept -> void;

    [[nodiscard]] inline auto region_of(lib::rb_node* node) noexcept -> vm_region* {
        return lib::container_of<vm_region, lib::rb_node, &vm_region::rb>(node);
    }

    using namespace zxfoundation::memory;

    [[nodiscard]] inline auto region_of(const lib::rb_node* node) noexcept -> const vm_region* {
        return lib::container_of<vm_region, lib::rb_node, &vm_region::rb>(node);
    }

    struct rb_insert_plan {
        lib::rb_node* parent{nullptr};
        lib::rb_node** link{nullptr};
        bool leftmost{false};
    };

    /// @brief Find a non-overlapping insertion position for an interval while
    ///        treating @p excluded as absent from the tree.
    [[nodiscard]] auto rb_plan_insert(
        vm_space& space, const vm_region& rgn, const vm_region* excluded = nullptr
    ) noexcept -> std::expected<rb_insert_plan, lib::kernel_error> {
        lib::rb_node** link = &space.tree.root.node;
        lib::rb_node* parent = nullptr;
        bool leftmost = true;

        while (*link) {
            parent = *link;
            const auto* cur = region_of(parent);
            if (cur == excluded) {
                link = &parent->right;
                leftmost = false;
                continue;
            }
            if (rgn.va_start < cur->va_start) {
                link = &parent->left;
            } else if (rgn.va_start >= cur->va_end) {
                link = &parent->right;
                leftmost = false;
            } else {
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::code::region_overlap));
            }
        }
        return rb_insert_plan{parent, link, leftmost};
    }

    auto rb_commit_insert(
        vm_space& space, vm_region& rgn, const rb_insert_plan& plan
    ) noexcept -> void {
        lib::rb_link_node(&rgn.rb, plan.parent, plan.link);
        lib::rb_insert_color_cached(&rgn.rb, space.tree, plan.leftmost);
        rgn.owning_space = &space;
    }

    /// @brief Insert a vm_region into the rb-tree, keyed by va_start.
    /// @return true on success, false if an overlapping region was found.
    auto rb_insert_region(vm_space& space, vm_region* rgn) noexcept -> bool {
        auto plan = rb_plan_insert(space, *rgn);
        if (!plan) return false;
        rb_commit_insert(space, *rgn, *plan);
        return true;
    }

    /// @brief RCU callback: free a vm_region back to the SCOMS table after a grace period.
    auto rcu_free_region_cb(sync::rcu::rcu_head* head) noexcept -> void {
        auto* rgn = lib::container_of<vm_region, sync::rcu::rcu_head,
            &vm_region::rcu>(head);
        discard_value g_region_table.release(rgn->ko.id);
    }

    /// @brief Remove a vm_region from the rb-tree and defer freeing via RCU.
    auto rb_remove_and_free(vm_space& space, vm_region* rgn) noexcept -> void {
        lib::rb_erase_cached(&rgn->rb, space.tree);
        space.total_mapped -= rgn->size();
        space.region_count -= 1;
        destroy_region_memory_objects(*rgn);
        discard_value scoms::object_core::detach(rgn->ko);
        sync::rcu::call_rcu(&rgn->rcu, rcu_free_region_cb);
    }

    /// @brief Discard a temporary split region without changing mapped bytes.
    /// Used only to reverse an interval split before the enclosing operation
    /// has removed any mapping or VMA state.
    auto rb_discard_split_region(vm_space& space, vm_region* rgn) noexcept -> void {
        lib::rb_erase_cached(&rgn->rb, space.tree);
        space.region_count -= 1;
        destroy_region_memory_objects(*rgn);
        discard_value scoms::object_core::detach(rgn->ko);
        sync::rcu::call_rcu(&rgn->rcu, rcu_free_region_cb);
    }

    /// @brief RCU-safe tree walk — each pointer load uses dereference.
    [[nodiscard]] auto rb_find_rcu(const vm_space& space, u64 va) noexcept -> vm_region* {
        lib::rb_node* node = sync::rcu::dereference(space.tree.root.node);
        while (node) {
            const auto* rgn = region_of(node);
            if (va < rgn->va_start) {
                node = sync::rcu::dereference(node->left);
            } else if (va >= rgn->va_end) {
                node = sync::rcu::dereference(node->right);
            } else {
                return const_cast<vm_region*>(rgn);
            }
        }
        return nullptr;
    }

    /// @brief Convert VME protection to MMU page_prot.
    [[nodiscard]] auto prot_to_mmu(vm_prot prot) noexcept
        -> arch::s390x::mmu::page_prot
    {
        using namespace arch::s390x::mmu;

        const bool can_write = !!(prot & vm_prot::write);
        const bool can_exec  = !!(prot & vm_prot::exec);

        u64 bits = 0;
        if (!can_write) bits |= PTE_PROTECT;
        if (!can_exec)  bits |= PTE_NOEXEC;

        return page_prot{bits};
    }

    /// @brief Convert VM protection and sharing flags into DGP access authority.
    [[nodiscard]] auto dgp_access_from_vm(
        vm_prot prot,
        vm_flags flags,
        vm_backing backing
    ) noexcept -> zxfoundation::dgp::memory_contract::memory_access {
        using namespace zxfoundation::dgp::memory_contract;

        memory_access access = memory_access::none;
        if (!!(prot & vm_prot::read)) access = access | memory_access::read;
        if (!!(prot & vm_prot::write)) access = access | memory_access::write;
        if (!!(prot & vm_prot::exec)) access = access | memory_access::execute;
        if (!!(flags & (vm_flags::portal | vm_flags::ipc)) ||
            backing == vm_backing::portal_shared || backing == vm_backing::ipc_shared) {
            access = access | memory_access::share;
        }
        return access;
    }

    /// @brief Convert VM sharing flags into a DGP memory-contract scope.
    [[nodiscard]] auto dgp_scope_from_vm(
        vm_flags flags,
        vm_backing backing
    ) noexcept -> zxfoundation::dgp::memory_contract::memory_scope {
        if (!!(flags & vm_flags::kernel)) return zxfoundation::dgp::memory_contract::memory_scope::nucleus;
        if (!!(flags & (vm_flags::portal | vm_flags::ipc)) ||
            backing == vm_backing::portal_shared || backing == vm_backing::ipc_shared) {
            return zxfoundation::dgp::memory_contract::memory_scope::portal_shared;
        }
        return zxfoundation::dgp::memory_contract::memory_scope::private_domain;
    }

    /// @brief Convert VM placement flags into the current DGP NUMA policy enum.
    [[nodiscard]] auto dgp_numa_policy_from_vm(vm_flags flags) noexcept
        -> zxfoundation::dgp::memory_contract::numa_policy {
        if (!!(flags & vm_flags::dma)) return zxfoundation::dgp::memory_contract::numa_policy::strict_node;
        return zxfoundation::dgp::memory_contract::numa_policy::deterministic_fallback;
    }

    /// @brief Build a complete DGP memory-contract request for a VM operation.
    [[nodiscard]] auto build_contract_request(
        const vm_space& space,
        u64 va_start,
        u64 length,
        vm_prot prot,
        vm_flags flags,
        vm_backing backing,
        u64 phys_base,
        u8 storage_key,
        u8 numa_node
    ) noexcept -> zxfoundation::dgp::memory_contract::contract_request {
        return zxfoundation::dgp::memory_contract::contract_request{
            .domain_id = space.owner_domain,
            .va_start = va_start,
            .va_end = va_start + length,
            .phys_start = phys_base,
            .length = length,
            .prot = prot_to_mmu(prot),
            .storage_key = storage_key == 0U ? space.storage_key : storage_key,
            .numa_node = numa_node == 0U ? space.numa_node : numa_node,
            .access = dgp_access_from_vm(prot, flags, backing),
            .scope = dgp_scope_from_vm(flags, backing),
            .policy = dgp_numa_policy_from_vm(flags),
        };
    }

    auto commit_contract_generation(
        vm_space& space,
        const zxfoundation::dgp::memory_contract::contract_result& contract
    ) noexcept -> void {
        space.dgp_generation = contract.generation;
    }

    [[nodiscard]] auto policy_from_contract(
        const zxfoundation::dgp::memory_contract::contract_result& contract,
        u64 mapping_generation, u32 storage_key_generation
    ) noexcept -> objects::memory_object_policy {
        return objects::memory_object_policy{
            .storage_key = contract.storage_key,
            .storage_key_generation = storage_key_generation,
            .numa_node = contract.numa_node,
            .dgp_scope = contract.scope,
            .dgp_access = contract.access,
            .dgp_rights = contract.required_rights,
            .invalidate = contract.invalidate,
            .dgp_generation = contract.generation,
            .mapping_generation = mapping_generation,
        };
    }

    [[nodiscard]] auto policy_from_region(
        const vm_region& rgn
    ) noexcept -> objects::memory_object_policy {
        return objects::memory_object_policy{
            .storage_key = rgn.storage_key(),
            .storage_key_generation = rgn.mo()
                ? rgn.mo()->storage_key_generation : 0U,
            .numa_node = rgn.numa_node(),
            .dgp_scope = rgn.dgp_scope(),
            .dgp_access = rgn.dgp_access(),
            .dgp_rights = rgn.dgp_rights(),
            .invalidate = rgn.invalidate(),
            .dgp_generation = rgn.dgp_generation(),
            .mapping_generation = rgn.mapping_generation(),
        };
    }

    [[nodiscard]] auto next_policy_from_contract(
        const vm_space& space,
        const zxfoundation::dgp::memory_contract::contract_result& contract
    ) noexcept -> objects::memory_object_policy {
        return policy_from_contract(contract, space.mapping_generation + 1U,
            space.storage_key_generation);
    }

    auto commit_region_policy(
        vm_space& space,
        vm_region& rgn,
        const zxfoundation::dgp::memory_contract::contract_result& contract
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const auto policy = next_policy_from_contract(space, contract);
        auto update_result = objects::memory_object_update_policy(
            rgn.mapping_object, policy);
        if (!update_result) return std::unexpected(update_result.error());
        space.mapping_generation = policy.mapping_generation;
        commit_contract_generation(space, contract);
        return {};
    }

    [[nodiscard]] auto contract_metadata_compatible(
        const vm_region& lhs,
        const vm_region& rhs
    ) noexcept -> bool {
        return lhs.storage_key() == rhs.storage_key()
            && lhs.numa_node()    == rhs.numa_node()
            && lhs.dgp_scope()    == rhs.dgp_scope()
            && lhs.dgp_access()   == rhs.dgp_access()
            && lhs.dgp_rights()   == rhs.dgp_rights()
            && lhs.dgp_generation() == rhs.dgp_generation();
    }

    [[nodiscard]] constexpr auto same_memory_ref(
        objects::memory_object_ref lhs, objects::memory_object_ref rhs
    ) noexcept -> bool {
        return lhs.type == rhs.type && lhs.id == rhs.id &&
            lhs.generation == rhs.generation;
    }

    /// @brief Apply the invalidation scope attached to a VM/DGP policy result.
    auto apply_vm_invalidation(
        u64 root_phys,
        const vm_region& rgn,
        zxfoundation::dgp::memory_contract::invalidation_scope scope
    ) noexcept -> void {
        using namespace arch::s390x::mmu;

        const dat_table<dat_level::region_1> root{root_phys};
        const u64 asce_val = asce_build(root).raw();
        tlb_apply_dgp_scope(scope, asce_val, 0ULL, rgn.va_start);
    }

    /// @brief Build a page-table allocation request from VM/DGP placement metadata.
    [[nodiscard]] auto table_policy_from_region(
        const vm_region& rgn
    ) noexcept -> pmm::allocation_request {
        return pmm::allocation_request{
            .order = 0U,
            .mem_zone = pmm::zone::normal,
            .use = pmm::purpose::page_table,
            .init = pmm::page_init::uninitialized,
            .placement = zxfoundation::memory::numa::allocation_policy{
                .preferred_node = rgn.numa_node(),
                .fallback = !!(rgn.flags & vm_flags::dma)
                    ? zxfoundation::memory::numa::fallback_mode::strict_local
                    : zxfoundation::memory::numa::fallback_mode::local_first,
                .require_dma31 = false,
                .allow_remote = !(!!(rgn.flags & vm_flags::dma)),
            },
        };
    }

    /// @brief Allocate a vm_region from the SCOMS table, initialize it, and return.
    [[nodiscard]] auto alloc_region(
        u64 va_start, u64 va_end,
        vm_prot prot, vm_flags flags,
        vm_backing backing, u64 phys_base
    ) noexcept -> std::expected<vm_region*, lib::kernel_error> {
        auto result = g_region_table.alloc();
        if (!result) return std::unexpected(result.error());

        auto embryo_ref = static_cast<
            scoms::kobject::robject<vm_region, base::lifecycle_state::embryo>&&
        >(result.value());

        vm_region* rgn = &(*embryo_ref);
        rgn->va_start = va_start;
        rgn->va_end   = va_end;
        rgn->prot     = prot;
        rgn->flags    = flags;
        rgn->backing  = backing;
        rgn->phys_base = phys_base;

        discard_value static_cast<
            scoms::kobject::robject<vm_region, base::lifecycle_state::embryo>&&
        >(embryo_ref).activate();

        return rgn;
    }

    [[nodiscard]] auto bind_region_owner(
        vm_region& rgn, u32 owner_domain
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto owner_result = scoms::object_core::set_owner(rgn.ko, owner_domain);
        if (!owner_result) return std::unexpected(owner_result.error());
        return {};
    }

    [[nodiscard]] auto attach_region_owner(
        vm_region& rgn, const vm_space& space
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (!space.address_space_object) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        auto attach_result = scoms::object_core::attach(
            rgn.ko, *space.address_space_object);
        if (!attach_result) return std::unexpected(attach_result.error());
        return {};
    }

    [[nodiscard]] auto create_region_mapping_object(
        vm_space& space, vm_region& rgn,
        objects::memory_object_ref grant,
        const objects::memory_object_policy& policy
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto grant_result = objects::memory_object_resolve(grant);
        if (!grant_result || (*grant_result)->ko.type != scoms::kobject::kobject_type::grant) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        auto mapping_result = objects::memory_object_create(
            scoms::kobject::kobject_type::mapping, space.owner_domain,
            &(*grant_result)->ko, rgn.va_start, rgn.size(),
            policy.dgp_rights, policy);
        if (!mapping_result) return std::unexpected(mapping_result.error());
        rgn.mapping_object = objects::memory_object_ref_of(**mapping_result);
        return {};
    }

    [[nodiscard]] auto create_region_memory_objects(
        vm_space& space, vm_region& rgn,
        const objects::memory_object_policy& policy
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto storage_result = objects::memory_object_create(
            scoms::kobject::kobject_type::storage, space.owner_domain,
            space.address_space_object, rgn.va_start, rgn.size(),
             dgp::isolation::isolation_rights::none, policy);
        if (!storage_result) return std::unexpected(storage_result.error());
        rgn.storage_object = objects::memory_object_ref_of(**storage_result);

        auto grant_result = objects::memory_object_create(
            scoms::kobject::kobject_type::grant, space.owner_domain,
            &(*storage_result)->ko, rgn.va_start, rgn.size(),
            policy.dgp_rights, policy);
        if (!grant_result) {
            discard_value objects::memory_object_destroy(rgn.storage_object);
            return std::unexpected(grant_result.error());
        }
        rgn.grant_object = objects::memory_object_ref_of(**grant_result);

        auto mapping_result = create_region_mapping_object(
            space, rgn, rgn.grant_object, policy);
        if (!mapping_result) {
            discard_value objects::memory_object_destroy(rgn.grant_object);
            discard_value objects::memory_object_destroy(rgn.storage_object);
            return std::unexpected(mapping_result.error());
        }
        return {};
    }

    auto destroy_region_memory_objects(vm_region& rgn) noexcept -> void {
        rgn.pages.destroy();
        if (rgn.mapped_extent) {
            discard_value objects::memory_extent_release_mapping(rgn.mapped_extent);
            rgn.mapped_extent = {};
        }
        if (rgn.mapping_object) {
            discard_value objects::memory_object_destroy(rgn.mapping_object);
            rgn.mapping_object = {};
        }
        if (rgn.grant_object) {
            auto grant = objects::memory_object_resolve(rgn.grant_object);
            if (grant && !scoms::object_core::has_dependents((*grant)->ko)) {
                discard_value objects::memory_object_destroy(rgn.grant_object);
            }
            rgn.grant_object = {};
        }
        if (rgn.storage_object) {
            auto storage = objects::memory_object_resolve(rgn.storage_object);
            if (storage && !scoms::object_core::has_dependents((*storage)->ko)) {
                discard_value objects::memory_object_destroy(rgn.storage_object);
            }
            rgn.storage_object = {};
        }
    }

    auto update_region_memory_object_ranges(vm_region& rgn) noexcept -> void {
        if (rgn.mapping_object) discard_value objects::memory_object_update_range(
            rgn.mapping_object, rgn.va_start, rgn.size());
    }

    /// @brief Map physical pages for a region [va_start, va_end) using identity phys_base.
    auto map_physical_range(
        u64 root_phys,
        const vm_region& rgn,
        u64 phys_base,
        arch::s390x::mmu::page_prot pp
    ) noexcept -> std::expected<void, lib::kernel_error> {
        using namespace arch::s390x::mmu;
        const dat_table<dat_level::region_1> root{root_phys};
        const auto table_policy = table_policy_from_region(rgn);
        u64 va   = rgn.va_start;
        u64 phys = phys_base;
        while (va < rgn.va_end) {
            auto r = map_page_with_policy<dat_level::region_1>(
                root, va, phys, pp, table_policy);
            if (!r) return std::unexpected(r.error());
            va   += PAGE_SIZE;
            phys += PAGE_SIZE;
        }
        return {};
    }

    /// @brief Release the pin owned by one resident VM slot.
    [[nodiscard]] auto release_slot_mapping(vm_page_slot& slot) noexcept -> bool {
        if (slot.state == vm_page_state::empty ||
            slot.state == vm_page_state::portal || slot.phys == 0ULL) return false;

        if (slot.cache_binding) {
            if (arch::s390x::cpu::skey::skey_is_changed(
                    arch::s390x::cpu::skey::iske(slot.phys))) {
                auto dirty = page_cache::mark_dirty_binding(slot.cache_binding);
                if (!dirty) {
                    sys::syschk::syschk_fatal(dirty.error(),
                        "vm: file-cache slot lost its cache binding");
                }
            }
            auto released = page_cache::release_binding(slot.cache_binding);
            if (!released) {
                sys::syschk::syschk_fatal(released.error(),
                    "vm: failed to release file-cache mapping lease");
            }
            return false;
        }

        auto released = pmm::frame_release_pin(slot.phys);
        if (!released) {
            sys::syschk::syschk_fatal(released.error(),
                "vm: failed to release resident frame pin");
        }
        return *released;
    }

    /// @brief Remove PTEs without guessing whether the mapped frame is PMM-owned.
    auto unmap_range_pages(
        u64 root_phys,
        u64 va_start, u64 va_end
    ) noexcept -> void {
        using namespace arch::s390x::mmu;
        const dat_table<dat_level::region_1> root{root_phys};
        u64 va = va_start;
        while (va < va_end) {
            discard_value unmap_page<dat_level::region_1>(root, va);
            va += PAGE_SIZE;
        }
    }

    [[nodiscard]] auto teardown_region_slots(
        vm_region& rgn
    ) noexcept -> u32 {
        if (!rgn.pages.slots) return 0U;
        u32 freed = 0U;
        for (u32 i = 0U; i < rgn.pages.count; ++i) {
            auto& slot = rgn.pages.slots[i];
            if (slot.state == vm_page_state::empty) continue;
            if (slot.state != vm_page_state::portal && slot.phys != 0ULL) {
                rmap::remove(rgn, i, slot.phys);
                if (release_slot_mapping(slot)) ++freed;
            }
            slot.clear();
        }
        return freed;
    }

    /// @brief Unlink every rmap entry belonging to @p rgn's resident
    ///        resident slots.
    auto region_unlink_rmap(vm_region& rgn) noexcept -> void {
        if (!rgn.pages.slots) return;
        for (u32 i = 0; i < rgn.pages.count; ++i) {
            const auto& slot = rgn.pages.slots[i];
            if (slot.state == vm_page_state::empty) continue;
            if (slot.state == vm_page_state::portal) continue;
            if (slot.phys == 0ULL) continue;
            rmap::remove(rgn, i, slot.phys);
        }
    }

    /// @brief Read committed count from a vm_region (handles atomic).
    [[nodiscard]] inline auto read_committed(const vm_region& rgn) noexcept -> u64 {
        return rgn.committed.load(std::memory_order::relaxed);
    }

    /// @brief Read resident count from a vm_region (handles atomic).
    [[nodiscard]] inline auto read_resident(const vm_region& rgn) noexcept -> u64 {
        return rgn.resident.load(std::memory_order::relaxed);
    }

    /// @brief Store committed count in a vm_region.
    inline auto store_committed(vm_region& rgn, u64 val) noexcept -> void {
        rgn.committed.store(val, std::memory_order::relaxed);
    }

    /// @brief Store resident count in a vm_region.
    inline auto store_resident(vm_region& rgn, u64 val) noexcept -> void {
        rgn.resident.store(val, std::memory_order::relaxed);
    }

    /// @brief Atomically add to committed.
    inline auto add_committed(vm_region& rgn, u64 delta) noexcept -> void {
        discard_value rgn.committed.fetch_add(delta, std::memory_order::relaxed);
    }

    /// @brief Atomically add to resident.
    inline auto add_resident(vm_region& rgn, u64 delta) noexcept -> void {
        discard_value rgn.resident.fetch_add(delta, std::memory_order::relaxed);
    }

    /// @brief Undo one successful @ref vm_split before either half is mutated.
    auto vm_join_split(
        vm_space& space, vm_region& lower, vm_region& upper
    ) noexcept -> void {
        if (lower.owning_space != &space || upper.owning_space != &space
            || lower.va_end != upper.va_start) {
            sys::syschk::syschk_fatal(
                lib::kernel_error::from_memory(lib::memory_error::purpose_mismatch),
                "vm: attempted to roll back unrelated VMA split");
        }

        const u64 upper_committed = read_committed(upper);
        const u64 upper_resident = read_resident(upper);
        if (upper.pages.slots) {
            for (u32 i = 0U; i < upper.pages.count; ++i) {
                auto& upper_slot = upper.pages.slots[i];
                if (upper_slot.state == vm_page_state::empty) continue;

                auto* lower_slot = lower.reserve_page(upper_slot.va_page);
                if (!lower_slot) {
                    sys::syschk::syschk_fatal(
                        lib::kernel_error::from_memory(lib::memory_error::out_of_memory),
                        "vm: split rollback exhausted original slot capacity");
                }
                upper_slot.transfer_to(*lower_slot);
                const u32 lower_index = static_cast<u32>(
                    lower_slot - lower.pages.slots);
                if (lower_slot->phys != 0ULL
                    && lower_slot->state != vm_page_state::portal
                    && !rmap::rebind(upper, i, lower, lower_index, lower_slot->phys)) {
                    sys::syschk::syschk_fatal(
                        lib::kernel_error::from_memory(lib::memory_error::purpose_mismatch),
                        "vm: split rollback lost reverse-map entry");
                }
            }
        }

        lower.va_end = upper.va_end;
        update_region_memory_object_ranges(lower);
        add_committed(lower, upper_committed);
        add_resident(lower, upper_resident);
        rb_discard_split_region(space, &upper);
    }

    auto register_vm_cap_lookup() noexcept -> void {
        discard_value zxfoundation::dgp::cap::register_lookup(
            zxfoundation::dgp::cap::cap_type::memory,
            [](u16 obj_id) noexcept -> scoms::kobject::bobject* {
                auto* object = objects::memory_object_resolve_id(
                    static_cast<u32>(obj_id));
                return object && object->ko.type == scoms::kobject::kobject_type::grant
                    ? &object->ko : nullptr;
            });
    }

} // namespace zxfoundation::memory::vm::detail

namespace zxfoundation::memory::vm {

    /// @brief Initialize a per-domain VME address space.
    auto vm_space_init(vm_space& space) noexcept -> void {
        space.tree       = {};
        space.total_mapped    = 0;
        space.total_committed = 0;
        space.total_resident  = 0;
        space.commit_limit_pages = VM_DEFAULT_COMMIT_LIMIT_PAGES;
        space.brk        = 0;
        space.mmap_base  = VM_MMAP_BASE;
        space.dgp_generation = 0;
        space.mapping_generation = 0;
        space.storage_key = 0;
        space.numa_node = 0;
        space.storage_key_generation = 0U;
        space.region_count = 0;
    }

    /// @brief Set a domain VM-space committed-page budget.
    /// @note A live budget may not be reduced below pages already committed.
    [[nodiscard]] auto vm_space_set_commit_limit(
        vm_space& space, u64 limit_pages
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const sync::irq_lock_guard guard{space.lock};
        if (limit_pages < space.total_committed) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::code::out_of_memory));
        }
        space.commit_limit_pages = limit_pages;
        return {};
    }

    /// @brief Atomically reserve committed pages for a caller that cannot
    ///        retain @c space.lock through its fallible page-install work.
    [[nodiscard]] auto vm_space_try_charge_commit(
        vm_space& space, u64 pages
    ) noexcept -> bool {
        const sync::irq_lock_guard guard{space.lock};
        if (!vm_commit_admitted(space, pages)) return false;
        space.total_committed += pages;
        return true;
    }

    /// @brief Return a page charge obtained from @ref vm_space_try_charge_commit.
    auto vm_space_uncharge_commit(vm_space& space, u64 pages) noexcept -> void {
        const sync::irq_lock_guard guard{space.lock};
        if (pages > space.total_committed) {
            sys::syschk::syschk_fatal(
                lib::kernel_error::from_memory(lib::memory_error::purpose_mismatch),
                "vm: commit-charge accounting underflow");
        }
        space.total_committed -= pages;
    }

    /// @brief RCU-protected lookup: find the vm_region containing VA.
    /// @return Pointer to the region, or nullptr if no region contains va.
    [[nodiscard]] auto vm_find(const vm_space& space, u64 va) noexcept -> vm_region* {
        sync::rcu::read_lock();
        auto* rgn = detail::rb_find_rcu(space, va);
        sync::rcu::read_unlock();
        return rgn;
    }

    /// @brief Find one live mapping view that consumes @p grant.
    [[nodiscard]] auto vm_find_grant_mapping(
        vm_space& space, objects::memory_object_ref grant
    ) noexcept -> vm_region* {
        if (!grant) return nullptr;
        const sync::irq_lock_guard guard{space.lock};
        for (lib::rb_node* node = lib::rb_first_cached(space.tree);
             node; node = lib::rb_next(node)) {
            auto* region = detail::region_of(node);
            if (region && detail::same_memory_ref(region->grant_object, grant)) {
                return region;
            }
        }
        return nullptr;
    }

    /// @brief Walk every active vm_region in the system via callback.
    /// @param[in] callback Function pointer invoked per region
    using region_visitor = bool (*)(vm_region&) noexcept;
    auto vm_for_each_region(region_visitor callback) noexcept -> void {
        if (!callback) return;
        for (u32 i = 0; i < MAX_VM_REGIONS; ++i) {
            auto* rgn = detail::g_region_table.find_raw(i);
            if (!rgn) continue;
            if (rgn->ko.state != base::lifecycle_state::active) continue;
            if (!callback(*rgn)) break;
        }
    }

    /// @brief Test if @p phys is mapped-resident in @p rgn.
    [[nodiscard]] auto vm_region_maps_phys(
        const vm_region& rgn, u64 phys
    ) noexcept -> bool {
        if (!rgn.pages.slots || rgn.pages.count == 0U) return false;
        if (!!(rgn.flags & vm_flags::pinned)) return false;
        for (u32 i = 0; i < rgn.pages.count; ++i) {
            const auto& slot = rgn.pages.slots[i];
        if (slot.state != vm_page_state::empty && slot.phys == phys)
            return true;
        }
        return false;
    }

    /// @brief Drop all resident page mappings in @p space whose backing
    ///        physical address lies in @c [phys_start, phys_end).
    /// @param[in,out] space  Address space to sweep (locked internally).
    /// @param[in] phys_start Inclusive start of physical range.
    /// @param[in] phys_end   Exclusive end of physical range.
    /// @return Number of physical frames freed (not slots dropped).
    [[nodiscard]] auto vm_space_drain_phys_range(
        vm_space& space, u64 phys_start, u64 phys_end
    ) noexcept -> u32 {
        if (phys_end <= phys_start) return 0U;

        sync::lock_guard g{space.lock};

        u32 freed = 0U;
        for (lib::rb_node* node = lib::rb_first_cached(space.tree);
             node; node = lib::rb_next(node)) {
            auto* rgn = detail::region_of(node);
            if (!rgn) continue;
            if (!!(rgn->flags & vm_flags::pinned)) continue;

            if (!rgn->pages.slots || rgn->pages.count == 0U) {
                // No slot array: physical regions reach this path.
                // Walk the hardware PTEs to find frames in range.
                if (rgn->backing == vm_backing::physical
                    && rgn->phys_base != 0ULL) {
                    const u64 rgn_phys_end =
                        rgn->phys_base + rgn->size();
                    if (rgn_phys_end <= phys_start
                        || rgn->phys_base >= phys_end) continue;
                    // Compute the overlap to drain.
                    const u64 drain_phys_start =
                        (rgn->phys_base > phys_start)
                            ? rgn->phys_base : phys_start;
                    const u64 drain_phys_end =
                        (rgn_phys_end < phys_end)
                            ? rgn_phys_end : phys_end;
                    const u64 drain_va_start =
                        rgn->va_start
                        + (drain_phys_start - rgn->phys_base);
                    const u64 drain_va_end =
                        rgn->va_start
                        + (drain_phys_end - rgn->phys_base);
                    detail::unmap_range_pages(
                        space.asce_val
                            & arch::s390x::mmu::ASCE_ORIGIN_MASK,
                        drain_va_start, drain_va_end);
                }
                continue;
            }

            for (u32 i = 0U; i < rgn->pages.count; ++i) {
                auto& slot = rgn->pages.slots[i];
                if (slot.state == vm_page_state::empty) continue;
                if (slot.state == vm_page_state::portal) continue;

                const u64 sp = slot.phys;
                if (sp < phys_start || sp >= phys_end) continue;

                discard_value arch::s390x::mmu::unmap_page_dispatch(
                    space.asce_val, slot.va_page);
                arch::s390x::mmu::tlb_flush_asce(space.asce_val);
                auto peers = rmap::invalidate_peers(sp, *rgn, i);
                if (!peers) {
                    sys::syschk::syschk_fatal(peers.error(),
                        "vm: failed to invalidate resident peers");
                }
                freed += peers->frames_released;
                rmap::remove(*rgn, i, slot.phys);
                if (detail::release_slot_mapping(slot)) ++freed;
                slot.clear();
                if (rgn->resident.load(std::memory_order::relaxed) > 0U)
                    rgn->resident.fetch_sub(1U, std::memory_order::relaxed);
                if (space.total_resident > 0U) --space.total_resident;
            }
        }

        if (freed > 0U && space.asce_val != 0U) {
            arch::s390x::mmu::tlb_flush_asce(space.asce_val);
        }
        return freed;
    }

    /// @brief Drain resident mappings whose phys lies in @c [phys_start,phys_end]
    /// @param[in] phys_start Inclusive start of physical range.
    /// @param[in] phys_end   Exclusive end of physical range.
    /// @return Number of physical frames freed.
    [[nodiscard]] auto vm_drain_phys_range(
        u64 phys_start, u64 phys_end
    ) noexcept -> u32 {
        if (phys_end <= phys_start) return 0U;

        u32 freed = 0U;
        auto cb = [&]([[maybe_unused]] vm_region& rgn) noexcept -> bool {
            if (!!(rgn.flags & vm_flags::pinned)) return true;

            if (!rgn.pages.slots || rgn.pages.count == 0U) {
                if (rgn.backing == vm_backing::physical
                    && rgn.phys_base != 0ULL) {
                    const u64 rgn_phys_end =
                        rgn.phys_base + rgn.size();
                    if (rgn_phys_end <= phys_start
                        || rgn.phys_base >= phys_end) return true;
                    const u64 drain_phys_start =
                        (rgn.phys_base > phys_start)
                            ? rgn.phys_base : phys_start;
                    const u64 drain_phys_end =
                        (rgn_phys_end < phys_end)
                            ? rgn_phys_end : phys_end;
                    const u64 drain_va_start =
                        rgn.va_start
                        + (drain_phys_start - rgn.phys_base);
                    const u64 drain_va_end =
                        rgn.va_start
                        + (drain_phys_end - rgn.phys_base);
                    const u64 k_asce =
                        arch::s390x::cpu::lowcore::get_current()->get<"kernel_asce">();
                    detail::unmap_range_pages(
                        k_asce & arch::s390x::mmu::ASCE_ORIGIN_MASK,
                        drain_va_start, drain_va_end);
                }
                return true;
            }

            for (u32 i = 0U; i < rgn.pages.count; ++i) {
                auto& slot = rgn.pages.slots[i];
                if (slot.state == vm_page_state::empty) continue;
                if (slot.state == vm_page_state::portal) continue;
                const u64 sp = slot.phys;
                if (sp < phys_start || sp >= phys_end) continue;

                const u64 k_asce = arch::s390x::cpu::lowcore::get_current()->get<"kernel_asce">();
                discard_value arch::s390x::mmu::unmap_page_dispatch(k_asce, slot.va_page);
                arch::s390x::mmu::tlb_flush_local();
                rmap::remove(rgn, i, slot.phys);
                if (detail::release_slot_mapping(slot)) ++freed;
                slot.clear();
                if (rgn.resident.load(std::memory_order::relaxed) > 0U)
                    rgn.resident.fetch_sub(1U, std::memory_order::relaxed);
            }
            return true;
        };

        for (u32 i = 0U; i < MAX_VM_REGIONS; ++i) {
            auto* rgn = detail::g_region_table.find_raw(i);
            if (!rgn) continue;
            if (rgn->ko.state != base::lifecycle_state::active) continue;
            cb(*rgn);
        }

        if (freed > 0U) {
            arch::s390x::mmu::tlb_flush_local();
        }
        return freed;
    }

    /// @param[in,out] space Address space whose tree is locked by the caller.
    /// @param[in,out] rgn Region whose start key changes.
    /// @param[in] new_start New page-aligned inclusive start address.
    /// @return Success when the new interval can be reinserted.
    [[nodiscard]] auto vm_rekey_region(
        vm_space& space, vm_region& rgn, u64 new_start
    ) noexcept -> bool {
        const u64 old_start = rgn.va_start;
        lib::rb_erase_cached(&rgn.rb, space.tree);
        rgn.va_start = new_start;
        if (detail::rb_insert_region(space, &rgn)) return true;
        rgn.va_start = old_start;
        discard_value detail::rb_insert_region(space, &rgn);
        return false;
    }

    /// @brief Top-down scan from mmap_base for a gap of at least size bytes.
    /// @param[in] space  The address space to scan.
    /// @param[in] size   Required gap size (must be page-aligned).
    /// @param[in] align  Alignment requirement (must be power of 2).
    /// @return  The base VA of the gap on success.
    [[nodiscard]] auto vm_find_gap(
        const vm_space& space, u64 size, u64 align
    ) noexcept -> std::expected<u64, lib::kernel_error> {
        if (size == 0)
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::code::invalid_arg));

        const u64 align_mask = align ? (align - 1) : 0;

        u64 gap_end = space.mmap_base;

        lib::rb_node* node = space.tree.root.node;
        lib::rb_node* candidate = nullptr;

        while (node) {
            const auto* rgn = detail::region_of(node);
            if (rgn->va_start < gap_end) {
                candidate = node;
                node = node->right;
            } else {
                node = node->left;
            }
        }

        while (candidate) {
            const auto* rgn = detail::region_of(candidate);

            if (gap_end > rgn->va_end) {
                const u64 aligned_start = (gap_end - size) & ~align_mask;
                if (aligned_start >= rgn->va_end && aligned_start >= VM_USER_START) {
                    return aligned_start;
                }
            }

            gap_end = rgn->va_start;
            candidate = lib::rb_prev(candidate);
        }

        if (gap_end > VM_USER_START) {
            const u64 aligned_start = (gap_end - size) & ~align_mask;
            if (aligned_start >= VM_USER_START && (aligned_start + size) <= gap_end) {
                return aligned_start;
            }
        }

        return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::code::out_of_memory));
    }

    /// @brief Try to merge rgn with its predecessor and/or successor.
    auto vm_try_merge(vm_space& space, vm_region* rgn) noexcept -> void {
        using namespace detail;

        if (!!(rgn->flags & vm_flags::no_merge) || rgn->pages.count != 0U) return;

        lib::rb_node* next_node = lib::rb_next(&rgn->rb);
        if (next_node) {
            auto* next = region_of(next_node);
            if (rgn->va_end == next->va_start &&
                rgn->prot    == next->prot &&
                rgn->flags   == next->flags &&
                rgn->backing == next->backing &&
                same_memory_ref(rgn->storage_object, next->storage_object) &&
                same_memory_ref(rgn->grant_object, next->grant_object) &&
                contract_metadata_compatible(*rgn, *next) &&
                !(next->flags & vm_flags::no_merge))
            {
                bool can_merge = true;
                if (rgn->backing == vm_backing::physical) {
                    // Physical ranges must be contiguous.
                    const u64 expected_phys = rgn->phys_base + rgn->size();
                    can_merge = (next->phys_base == expected_phys);
                }
                if (can_merge && next->pages.count == 0U) {
                    // Absorb successor into rgn.
                    rgn->va_end = next->va_end;
                    update_region_memory_object_ranges(*rgn);
                    if (next->mapping_generation() > rgn->mapping_generation()) {
                        rgn->set_mapping_generation(next->mapping_generation());
                    }
                    const u64 nc = read_committed(*next);
                    const u64 nr = read_resident(*next);
                    add_committed(*rgn, nc);
                    add_resident(*rgn, nr);
                    rb_remove_and_free(space, next);
                }
            }
        }

        lib::rb_node* prev_node = lib::rb_prev(&rgn->rb);
        if (prev_node) {
            auto* prev = region_of(prev_node);
            if (prev->va_end == rgn->va_start &&
                prev->prot    == rgn->prot &&
                prev->flags   == rgn->flags &&
                prev->backing == rgn->backing &&
                same_memory_ref(prev->storage_object, rgn->storage_object) &&
                same_memory_ref(prev->grant_object, rgn->grant_object) &&
                contract_metadata_compatible(*prev, *rgn) &&
                !(prev->flags & vm_flags::no_merge))
            {
                bool can_merge = true;
                if (prev->backing == vm_backing::physical) {
                    const u64 expected_phys = prev->phys_base + prev->size();
                    can_merge = (rgn->phys_base == expected_phys);
                }
                if (can_merge && prev->pages.count == 0U) {
                    // Absorb rgn into predecessor.
                    prev->va_end = rgn->va_end;
                    update_region_memory_object_ranges(*prev);
                    if (rgn->mapping_generation() > prev->mapping_generation()) {
                        prev->set_mapping_generation(rgn->mapping_generation());
                    }
                    const u64 rc = read_committed(*rgn);
                    const u64 rr = read_resident(*rgn);
                    add_committed(*prev, rc);
                    add_resident(*prev, rr);
                    rb_remove_and_free(space, rgn);
                }
            }
        }
    }

    /// @brief Split rgn at split_va into [rgn->va_start, split_va) and [split_va, old_end).
    /// @return Pointer to the new region [split_va, old_end) on success.
    [[nodiscard]] auto vm_split(
        vm_space& space, vm_region* rgn, u64 split_va
    ) noexcept -> std::expected<vm_region*, lib::kernel_error> {
        using namespace detail;

        if (split_va <= rgn->va_start || split_va >= rgn->va_end)
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::code::invalid_arg));
        if ((split_va & (PAGE_SIZE - 1)) != 0)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::code::alignment_error));

        // Allocate a new region for the upper half.
        const u64 old_end = rgn->va_end;
        const u64 old_size = rgn->size();
        const u64 upper_size = old_end - split_va;
        const u64 lower_size = split_va - rgn->va_start;

        const u64 orig_committed = read_committed(*rgn);
        const u64 orig_resident  = read_resident(*rgn);

        if (space.region_count >= space.max_regions) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::out_of_memory));
        }

        // Compute physical base for the upper half (physical backing).
        u64 upper_phys = 0;
        if (rgn->backing == vm_backing::physical) {
            upper_phys = rgn->phys_base + lower_size;
        }

        auto new_result = alloc_region(
            split_va, old_end, rgn->prot, rgn->flags,
            rgn->backing, upper_phys);
        if (!new_result) return std::unexpected(new_result.error());

        vm_region* new_rgn = *new_result;
        auto owner_result = bind_region_owner(*new_rgn, rgn->owner_domain);
        if (!owner_result) {
            discard_value scoms::object_core::detach(new_rgn->ko);
            discard_value g_region_table.release(new_rgn->ko.id);
            return std::unexpected(owner_result.error());
        }
        auto attach_result = attach_region_owner(*new_rgn, space);
        if (!attach_result) {
            discard_value scoms::object_core::detach(new_rgn->ko);
            discard_value g_region_table.release(new_rgn->ko.id);
            return std::unexpected(attach_result.error());
        }
        new_rgn->portal_id     = rgn->portal_id;
        new_rgn->owner_domain  = rgn->owner_domain;
        new_rgn->file_node = rgn->file_node;
        new_rgn->file_page_offset = rgn->file_page_offset
            + static_cast<u32>((split_va - rgn->va_start) >> PAGE_SHIFT);
        const auto policy = policy_from_region(*rgn);
        new_rgn->storage_object = rgn->storage_object;
        new_rgn->grant_object = rgn->grant_object;
        auto object_result = create_region_mapping_object(
            space, *new_rgn, new_rgn->grant_object, policy);
        if (!object_result) {
            discard_value scoms::object_core::detach(new_rgn->ko);
            discard_value g_region_table.release(new_rgn->ko.id);
            return std::unexpected(object_result.error());
        }
        if (rgn->mapped_extent) {
            auto extent_result = objects::memory_extent_acquire_mapping(
                rgn->mapped_extent);
            if (!extent_result) {
                destroy_region_memory_objects(*new_rgn);
                discard_value scoms::object_core::detach(new_rgn->ko);
                discard_value g_region_table.release(new_rgn->ko.id);
                return std::unexpected(extent_result.error());
            }
            new_rgn->mapped_extent = rgn->mapped_extent;
        }

        auto insert_plan = rb_plan_insert(space, *new_rgn, rgn);
        if (!insert_plan) {
            destroy_region_memory_objects(*new_rgn);
            discard_value scoms::object_core::detach(new_rgn->ko);
            discard_value g_region_table.release(new_rgn->ko.id);
            return std::unexpected(insert_plan.error());
        }

        if (!new_rgn->pages.reserve_capacity(rgn->pages.count)) {
            destroy_region_memory_objects(*new_rgn);
            discard_value scoms::object_core::detach(new_rgn->ko);
            discard_value g_region_table.release(new_rgn->ko.id);
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::out_of_memory));
        }

        u64 upper_committed = 0;
        u64 upper_resident = 0;
        if (rgn->pages.slots) {
            for (u32 i = 0; i < rgn->pages.count; ++i) {
                const auto& old_slot = rgn->pages.slots[i];
                if (old_slot.state == vm_page_state::empty ||
                    old_slot.va_page < split_va) continue;

                auto* new_slot = new_rgn->reserve_page(old_slot.va_page);
                if (!new_slot) [[unlikely]] {
                    sys::syschk::syschk_fatal(
                        lib::kernel_error::from_memory(lib::memory_error::out_of_memory),
                        "vm: preallocated split slot unexpectedly unavailable");
                }
                rgn->pages.slots[i].transfer_to(*new_slot);
                const u32 new_index = static_cast<u32>(new_slot - new_rgn->pages.slots);
                if (new_slot->phys != 0ULL &&
                    new_slot->state != vm_page_state::portal &&
                    !rmap::rebind(*rgn, i, *new_rgn, new_index, new_slot->phys)) {
                    sys::syschk::syschk_fatal(
                        lib::kernel_error::from_memory(lib::memory_error::purpose_mismatch),
                        "vm: missing rmap entry while committing a VMA split");
                }
                if (new_slot->state != vm_page_state::empty) ++upper_resident;
                if (new_slot->state == vm_page_state::resident ||
                    new_slot->state == vm_page_state::cow ||
                    new_slot->state == vm_page_state::ipc) {
                    ++upper_committed;
                }
            }
            while (rgn->pages.count != 0U &&
                   rgn->pages.slots[rgn->pages.count - 1U].state == vm_page_state::empty) {
                --rgn->pages.count;
            }
        } else {
            const u64 upper_pages = upper_size >> PAGE_SHIFT;
            const u64 total_pages = old_size >> PAGE_SHIFT;
            if (total_pages != 0U) {
                upper_committed = (orig_committed * upper_pages) / total_pages;
                upper_resident = (orig_resident * upper_pages) / total_pages;
            }
        }

        rgn->va_end = split_va;
        rb_commit_insert(space, *new_rgn, *insert_plan);
        update_region_memory_object_ranges(*rgn);
        store_committed(*new_rgn, upper_committed);
        store_resident(*new_rgn, upper_resident);
        store_committed(*rgn, orig_committed - upper_committed);
        store_resident(*rgn, orig_resident - upper_resident);
        space.region_count += 1;

        return new_rgn;
    }

    /// @brief Map a region of size bytes with given protection and flags.
    /// @return The virtual address of the mapped region on success.
    [[nodiscard]] auto vm_map(
        vm_space& space,
        u64 size, vm_prot prot, vm_flags flags
    ) noexcept -> std::expected<u64, lib::kernel_error> {
        using namespace detail;

        if (size == 0 || (size & (PAGE_SIZE - 1)) != 0)
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::code::invalid_arg));

        const sync::irq_lock_guard guard{space.lock};

        if (space.region_count >= space.max_regions)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::code::out_of_memory));

        auto gap = vm_find_gap(space, size, PAGE_SIZE);
        if (!gap) return std::unexpected(gap.error());
        const u64 va = *gap;

        const vm_backing requested_backing =
            (flags & vm_flags::file) != vm_flags::none ? vm_backing::file :
            ((flags & vm_flags::ipc) != vm_flags::none ? vm_backing::ipc_shared :
             vm_backing::anonymous);
        const auto request = build_contract_request(
            space, va, size, prot, flags, requested_backing,
            0ULL, 0U, 0U);
        auto contract_result = zxfoundation::dgp::memory_contract::authorize_mapping(request);
        if (!contract_result) return std::unexpected(contract_result.error());

        auto rgn_r = alloc_region(va, va + size, prot, flags,
            requested_backing, 0);
        if (!rgn_r) return std::unexpected(rgn_r.error());
        vm_region* rgn = *rgn_r;
        rgn->owner_domain = space.owner_domain;
        auto owner_result = bind_region_owner(*rgn, space.owner_domain);
        if (!owner_result) {
            discard_value scoms::object_core::detach(rgn->ko);
            discard_value g_region_table.release(rgn->ko.id);
            return std::unexpected(owner_result.error());
        }
        auto attach_result = attach_region_owner(*rgn, space);
        if (!attach_result) {
            discard_value scoms::object_core::detach(rgn->ko);
            discard_value g_region_table.release(rgn->ko.id);
            return std::unexpected(attach_result.error());
        }
        const auto policy = next_policy_from_contract(space, *contract_result);
        auto object_result = create_region_memory_objects(space, *rgn, policy);
        if (!object_result) {
            discard_value scoms::object_core::detach(rgn->ko);
            discard_value g_region_table.release(rgn->ko.id);
            return std::unexpected(object_result.error());
        }
        space.mapping_generation = policy.mapping_generation;
        commit_contract_generation(space, *contract_result);

        if (!rb_insert_region(space, rgn)) {
            destroy_region_memory_objects(*rgn);
            discard_value scoms::object_core::detach(rgn->ko);
            discard_value g_region_table.release(rgn->ko.id);
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::code::region_overlap));
        }

        space.total_mapped += size;
        space.region_count += 1;

        vm_try_merge(space, rgn);

        return va;
    }

    /// @brief Map at an exact VA with specified backing.
    [[nodiscard]] auto vm_map_fixed(
        vm_space& space,
        u64 va, u64 size,
        vm_prot prot, vm_flags flags,
        vm_backing backing, u64 phys_base,
        objects::memory_object_ref mapped_extent
    ) noexcept -> std::expected<void, lib::kernel_error> {
        using namespace detail;

        if (size == 0 || (size & (PAGE_SIZE - 1)) != 0)
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::code::invalid_arg));
        if ((va & (PAGE_SIZE - 1)) != 0)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::code::alignment_error));

        if (va < VM_USER_START || va >= VM_USER_END || va + size < va || va + size > VM_USER_END)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::invalid_address));
        if ((prot & vm_prot::write) != vm_prot::none && (prot & vm_prot::exec) != vm_prot::none)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));

        if (mapped_extent && backing != vm_backing::physical) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        if (mapped_extent) {
            auto extent = objects::memory_object_resolve(mapped_extent);
            if (!extent || (*extent)->ko.type != scoms::kobject::kobject_type::memory_extent ||
                phys_base < (*extent)->range_start ||
                phys_base + size < phys_base ||
                phys_base + size > (*extent)->range_start + (*extent)->range_length) {
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::capability_invalid));
            }
            auto frame_check = pmm::frame_validate_mapping(
                phys_base, size, mapped_extent.id, mapped_extent.generation);
            if (!frame_check) return std::unexpected(frame_check.error());
        }

        const sync::irq_lock_guard guard{space.lock};

        const auto request = build_contract_request(
            space, va, size, prot, flags, backing, phys_base, 0U, 0U);
        auto contract_result = zxfoundation::dgp::memory_contract::authorize_mapping(request);
        if (!contract_result) return std::unexpected(contract_result.error());

        if (space.region_count >= space.max_regions)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::code::out_of_memory));

        const u64 num_pages = size >> PAGE_SHIFT;
        if (backing == vm_backing::physical && phys_base != 0U
            && !vm_commit_admitted(space, num_pages)) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::code::out_of_memory));
        }

        auto rgn_r = alloc_region(va, va + size, prot, flags, backing, phys_base);
        if (!rgn_r) return std::unexpected(rgn_r.error());
        vm_region* rgn = *rgn_r;
        rgn->owner_domain = space.owner_domain;
        auto owner_result = bind_region_owner(*rgn, space.owner_domain);
        if (!owner_result) {
            discard_value scoms::object_core::detach(rgn->ko);
            discard_value g_region_table.release(rgn->ko.id);
            return std::unexpected(owner_result.error());
        }
        auto attach_result = attach_region_owner(*rgn, space);
        if (!attach_result) {
            discard_value scoms::object_core::detach(rgn->ko);
            discard_value g_region_table.release(rgn->ko.id);
            return std::unexpected(attach_result.error());
        }
        const auto policy = next_policy_from_contract(space, *contract_result);
        auto object_result = create_region_memory_objects(space, *rgn, policy);
        if (!object_result) {
            discard_value scoms::object_core::detach(rgn->ko);
            discard_value g_region_table.release(rgn->ko.id);
            return std::unexpected(object_result.error());
        }
        if (mapped_extent) {
            auto retain_extent = objects::memory_extent_acquire_mapping(mapped_extent);
            if (!retain_extent) {
                destroy_region_memory_objects(*rgn);
                discard_value scoms::object_core::detach(rgn->ko);
                discard_value g_region_table.release(rgn->ko.id);
                return std::unexpected(retain_extent.error());
            }
            rgn->mapped_extent = mapped_extent;
        }
        space.mapping_generation = policy.mapping_generation;
        commit_contract_generation(space, *contract_result);

        if (!rb_insert_region(space, rgn)) {
            destroy_region_memory_objects(*rgn);
            discard_value scoms::object_core::detach(rgn->ko);
            discard_value g_region_table.release(rgn->ko.id);
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::code::region_overlap));
        }

        space.total_mapped += size;
        space.region_count += 1;

        if (backing == vm_backing::physical && phys_base != 0) {
            store_committed(*rgn, num_pages);
            store_resident(*rgn, num_pages);
            space.total_committed += num_pages;
            space.total_resident  += num_pages;
        }

        vm_try_merge(space, rgn);

        return {};
    }

    [[nodiscard]] auto vm_unmap(
        vm_space& space, u64 root_phys, u64 va, u64 size
    ) noexcept -> std::expected<void, lib::kernel_error>;

    /// @brief Map a checked subrange of a capability-bound physical extent.
    /// The caller supplies an offset, never an arbitrary physical address.
    [[nodiscard]] auto vm_map_extent(
        vm_space& space, u64 root_phys, u64 va, u64 length, vm_prot prot,
        objects::memory_object_ref extent_ref, u64 extent_offset
    ) noexcept -> std::expected<void, lib::kernel_error> {
        using namespace detail;

        if (length == 0U || (va & (PAGE_SIZE - 1U)) != 0U ||
            (length & (PAGE_SIZE - 1U)) != 0U ||
            (extent_offset & (PAGE_SIZE - 1U)) != 0U ||
            extent_offset + length < extent_offset) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::invalid_address));
        }

        auto extent_result = objects::memory_object_resolve(extent_ref);
        if (!extent_result || (*extent_result)->ko.type != scoms::kobject::kobject_type::memory_extent ||
            extent_offset + length > (*extent_result)->range_length) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        const u64 phys_base = (*extent_result)->range_start + extent_offset;
        if (phys_base + length < phys_base) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::invalid_address));
        }
        auto frame_check = pmm::frame_validate_mapping(
            phys_base, length, extent_ref.id, extent_ref.generation);
        if (!frame_check) return std::unexpected(frame_check.error());

        auto vma_result = vm_map_fixed(
            space, va, length, prot,
            vm_flags::fixed | vm_flags::physical | vm_flags::user | vm_flags::no_merge,
            vm_backing::physical, phys_base, extent_ref);
        if (!vma_result) return std::unexpected(vma_result.error());

        auto* region = vm_find(space, va);
        if (!region) {
            discard_value vm_unmap(space, root_phys, va, length);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));
        }

        const auto pte_result = map_physical_range(
            root_phys, *region, phys_base, prot_to_mmu(prot));
        if (!pte_result) {
            unmap_range_pages(root_phys, va, va + length);
            discard_value vm_unmap(space, root_phys, va, length);
            return std::unexpected(pte_result.error());
        }

        for (u64 phys = phys_base; phys < phys_base + length; phys += PAGE_SIZE) {
            arch::s390x::cpu::skey::sske_preserve_rc(
                phys, region->storage_key(), false);
        }
        return {};
    }

    [[nodiscard]] auto vm_unmap(
        vm_space& space, u64 root_phys, u64 va, u64 size
    ) noexcept -> std::expected<void, lib::kernel_error>;

    [[nodiscard]] auto vm_map_shared_region(
        vm_space& target, u64 target_root_phys, u64 target_va, u64 length,
        vm_region& source, vm_prot prot
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (length == 0 || (target_va & (PAGE_SIZE - 1)) != 0 ||
            (length & (PAGE_SIZE - 1)) != 0 || length > source.size())
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));

        if (source.owner_domain != target.owner_domain) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }

        auto map_result = vm_map_fixed(
            target, target_va, length, prot,
            vm_flags::ipc | vm_flags::shared_read,
            vm_backing::ipc_shared, 0);
        if (!map_result) return std::unexpected(map_result.error());

        auto* target_region = vm_find(target, target_va);
        if (!target_region) {
            discard_value vm_unmap(target, target_root_phys, target_va, length);
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));
        }
        target_region->file_node = &source;
        for (u64 offset = 0; offset < length; offset += PAGE_SIZE) {
            auto* source_slot = source.find_page(source.va_start + offset);
            if (!source_slot || source_slot->phys == 0) continue;
            if (!vm_space_try_charge_commit(target, 1U)) {
                discard_value vm_unmap(target, target_root_phys, target_va, length);
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::code::out_of_memory));
            }
            const auto release_commit = [&]() noexcept -> void {
                vm_space_uncharge_commit(target, 1U);
            };
            page_cache::cache_binding cache_binding{};
            if (source_slot->cache_binding) {
                auto duplicated = page_cache::duplicate_binding(
                    source_slot->cache_binding, target_region->storage_key());
                if (!duplicated) {
                    release_commit();
                    discard_value vm_unmap(target, target_root_phys, target_va, length);
                    return std::unexpected(duplicated.error());
                }
                cache_binding = *duplicated;
            } else {
                auto ref = pmm::frame_ref(source_slot->phys);
                if (!ref) {
                    release_commit();
                    discard_value vm_unmap(target, target_root_phys, target_va, length);
                    return std::unexpected(ref.error());
                }
            }
            auto release_new_pin = [&]() noexcept -> void {
                if (cache_binding) {
                    discard_value page_cache::release_binding(cache_binding);
                } else {
                    discard_value pmm::frame_release_pin(source_slot->phys);
                }
            };
            auto* target_slot = target_region->reserve_page(target_va + offset);
            if (!target_slot) {
                release_new_pin();
                release_commit();
                discard_value vm_unmap(target, target_root_phys, target_va, length);
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::out_of_memory));
            }
            target_slot->phys = source_slot->phys;
            target_slot->state = vm_page_state::ipc;
            target_slot->flags = 1U;
            target_slot->cache_binding = cache_binding;
            const u32 target_slot_index = static_cast<u32>(
                target_slot - target_region->pages.slots);
            const u32 source_slot_index = static_cast<u32>(
                source_slot - source.pages.slots);
            auto source_rmap = rmap::insert(const_cast<vm_region&>(source),
                source_slot_index, source_slot->phys);
            if (!source_rmap) {
                target_slot->cache_binding.clear();
                target_slot->clear();
                release_new_pin();
                release_commit();
                discard_value vm_unmap(target, target_root_phys, target_va, length);
                return std::unexpected(source_rmap.error());
            }
            auto map_result = arch::s390x::mmu::map_page<
                arch::s390x::mmu::dat_level::region_1>(
                    arch::s390x::mmu::dat_table<
                        arch::s390x::mmu::dat_level::region_1>{target_root_phys},
                    target_va + offset, source_slot->phys,
                    detail::prot_to_mmu(vm_prot::read));
            if (!map_result) {
                target_slot->cache_binding.clear();
                target_slot->clear();
                release_new_pin();
                release_commit();
                discard_value vm_unmap(target, target_root_phys, target_va, length);
                return std::unexpected(map_result.error());
            }
            auto target_rmap = rmap::insert(*target_region, target_slot_index,
                source_slot->phys);
            if (!target_rmap) {
                target_slot->cache_binding.clear();
                target_slot->clear();
                discard_value arch::s390x::mmu::unmap_page<
                    arch::s390x::mmu::dat_level::region_1>(
                        arch::s390x::mmu::dat_table<
                        arch::s390x::mmu::dat_level::region_1>{target_root_phys},
                        target_va + offset);
                release_new_pin();
                release_commit();
                discard_value vm_unmap(target, target_root_phys, target_va, length);
                return std::unexpected(target_rmap.error());
            }
            discard_value target_region->resident.fetch_add(1, std::memory_order::relaxed);
            discard_value target_region->committed.fetch_add(1, std::memory_order::relaxed);
            target.total_resident += 1;
        }
        return {};
    }

    /// @brief Unmap pages in [va, va+size), splitting partially-overlapping
    ///        regions, removing fully-covered regions, and freeing physical pages.
    [[nodiscard]] auto vm_unmap(
        vm_space& space,
        u64 root_phys,
        u64 va, u64 size
    ) noexcept -> std::expected<void, lib::kernel_error> {
        using namespace detail;

        if (size == 0 || (size & (PAGE_SIZE - 1)) != 0)
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::code::invalid_arg));
        if ((va & (PAGE_SIZE - 1)) != 0)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::code::alignment_error));

        if (va + size < va) return std::unexpected(lib::kernel_error::from_memory(
            lib::memory_error::invalid_address));
        const u64 unmap_end = va + size;
        auto contract_result = zxfoundation::dgp::memory_contract::validate_range(va, unmap_end);
        if (!contract_result) return std::unexpected(contract_result.error());

        const sync::irq_lock_guard guard{space.lock};

        // Find the first region that could overlap [va, unmap_end).
        lib::rb_node* node = space.tree.root.node;
        lib::rb_node* first_overlap = nullptr;

        while (node) {
            const auto* rgn = region_of(node);
            if (unmap_end <= rgn->va_start) {
                node = node->left;
            } else if (va >= rgn->va_end) {
                node = node->right;
            } else {
                // Overlaps — but find the leftmost overlapping region.
                first_overlap = node;
                node = node->left;
            }
        }

        if (!first_overlap) return {};

        node = first_overlap;
        while (node) {
            auto* rgn = region_of(node);
            if (rgn->va_start >= unmap_end) break;
            const u64 clip_start = std::max(rgn->va_start, va);
            const u64 clip_end = std::min(rgn->va_end, unmap_end);
            u64 clip_phys = rgn->phys_base;
            if (rgn->backing == vm_backing::physical) {
                clip_phys += clip_start - rgn->va_start;
            }
            const auto request = build_contract_request(
                space, clip_start, clip_end - clip_start,
                rgn->prot, rgn->flags, rgn->backing,
                clip_phys, rgn->storage_key(), rgn->numa_node());
            auto unmap_result = zxfoundation::dgp::memory_contract::authorize_unmap(request);
            if (!unmap_result) return std::unexpected(unmap_result.error());
            node = lib::rb_next(node);
        }

        vm_region* split_lower = nullptr;
        vm_region* split_upper = nullptr;
        auto* start_region = vm_find(space, va);
        if (start_region && start_region->va_start < va) {
            auto split_r = vm_split(space, start_region, va);
            if (!split_r) return std::unexpected(split_r.error());
            split_lower = start_region;
            split_upper = *split_r;
        }

        auto* end_region = vm_find(space, unmap_end - PAGE_SIZE);
        if (end_region && end_region->va_end > unmap_end) {
            auto split_r = vm_split(space, end_region, unmap_end);
            if (!split_r) {
                if (split_lower && split_upper) {
                    detail::vm_join_split(space, *split_lower, *split_upper);
                }
                return std::unexpected(split_r.error());
            }
        }

        node = space.tree.root.node;
        first_overlap = nullptr;
        while (node) {
            const auto* rgn = region_of(node);
            if (unmap_end <= rgn->va_start) {
                node = node->left;
            } else if (va >= rgn->va_end) {
                node = node->right;
            } else {
                first_overlap = node;
                node = node->left;
            }
        }

        node = first_overlap;
        while (node) {
            auto* rgn = region_of(node);
            lib::rb_node* next = lib::rb_next(node);
            if (rgn->va_start >= unmap_end) break;

            const auto request = build_contract_request(
                space, rgn->va_start, rgn->size(), rgn->prot, rgn->flags,
                rgn->backing, rgn->phys_base, rgn->storage_key(), rgn->numa_node());
            auto unmap_result = zxfoundation::dgp::memory_contract::authorize_unmap(request);
            if (!unmap_result) {
                sys::syschk::syschk_fatal(unmap_result.error(),
                    "vm: prevalidated unmap authorization changed during commit");
            }
            rgn->set_invalidate(unmap_result->invalidate);
            unmap_range_pages(root_phys, rgn->va_start, rgn->va_end);
            apply_vm_invalidation(root_phys, *rgn, rgn->invalidate());
            discard_value detail::teardown_region_slots(*rgn);

            space.total_committed -= read_committed(*rgn);
            space.total_resident  -= read_resident(*rgn);

            rb_remove_and_free(space, rgn);

            node = next;
        }

        return {};
    }

    /// @brief Change protection bits for [va, va+size).
    [[nodiscard]] auto vm_protect(
        vm_space& space,
        u64 root_phys,
        u64 va, u64 size, vm_prot new_prot
    ) noexcept -> std::expected<void, lib::kernel_error> {
        using namespace detail;
        using namespace arch::s390x::mmu;

        if (size == 0 || (size & (PAGE_SIZE - 1)) != 0)
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::code::invalid_arg));
        if ((va & (PAGE_SIZE - 1)) != 0)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::code::alignment_error));

        auto contract_result = zxfoundation::dgp::memory_contract::validate_range(va, va + size);
        if (!contract_result) return std::unexpected(contract_result.error());

        if (va + size < va || va < VM_USER_START || va + size > VM_USER_END)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::invalid_address));
        if ((new_prot & vm_prot::write) != vm_prot::none
            && (new_prot & vm_prot::exec) != vm_prot::none)
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));

        const sync::irq_lock_guard guard{space.lock};

        const u64 prot_end = va + size;

        // Convert vm_prot to hardware page_prot.
        u64 hw_bits = 0;
        if (!(new_prot & vm_prot::write))
            hw_bits |= PTE_PROTECT;   // Read-only
        if (!(new_prot & vm_prot::exec))
            hw_bits |= PTE_NOEXEC;    // No-execute
        const page_prot hw_prot{hw_bits};
        const dat_table<dat_level::region_1> root{root_phys};

        // Find the leftmost overlap in [va, prot_end).
        lib::rb_node* node = space.tree.root.node;
        lib::rb_node* first_overlap = nullptr;

        while (node) {
            const auto* rgn = region_of(node);
            if (prot_end <= rgn->va_start) {
                node = node->left;
            } else if (va >= rgn->va_end) {
                node = node->right;
            } else {
                first_overlap = node;
                node = node->left;
            }
        }

        if (!first_overlap)
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::code::not_found));

        node = first_overlap;
        while (node) {
            auto* rgn = region_of(node);
            if (rgn->va_start >= prot_end) break;
            const u64 clip_start = std::max(rgn->va_start, va);
            const u64 clip_end = std::min(rgn->va_end, prot_end);
            u64 clip_phys = rgn->phys_base;
            if (rgn->backing == vm_backing::physical) {
                clip_phys += clip_start - rgn->va_start;
            }
            const arch::s390x::mmu::page_prot old_prot = prot_to_mmu(rgn->prot);
            const auto request = build_contract_request(
                space, clip_start, clip_end - clip_start, new_prot, rgn->flags,
                rgn->backing, clip_phys, rgn->storage_key(), rgn->numa_node());
            auto protect_result = zxfoundation::dgp::memory_contract::authorize_protect(
                request, old_prot);
            if (!protect_result) return std::unexpected(protect_result.error());
            if (!objects::memory_object_resolve(rgn->mapping_object)) {
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::capability_invalid));
            }
            node = lib::rb_next(node);
        }

        vm_region* split_lower = nullptr;
        vm_region* split_upper = nullptr;
        auto* start_region = vm_find(space, va);
        if (start_region && start_region->va_start < va) {
            auto split_result = vm_split(space, start_region, va);
            if (!split_result) return std::unexpected(split_result.error());
            split_lower = start_region;
            split_upper = *split_result;
        }

        auto* end_region = vm_find(space, prot_end - PAGE_SIZE);
        if (end_region && end_region->va_end > prot_end) {
            auto split_result = vm_split(space, end_region, prot_end);
            if (!split_result) {
                if (split_lower && split_upper) {
                    vm_join_split(space, *split_lower, *split_upper);
                }
                return std::unexpected(split_result.error());
            }
        }

        node = space.tree.root.node;
        first_overlap = nullptr;
        while (node) {
            const auto* rgn = region_of(node);
            if (prot_end <= rgn->va_start) {
                node = node->left;
            } else if (va >= rgn->va_end) {
                node = node->right;
            } else {
                first_overlap = node;
                node = node->left;
            }
        }

        node = first_overlap;
        while (node) {
            auto* rgn = region_of(node);
            if (rgn->va_start >= prot_end) break;

            const arch::s390x::mmu::page_prot old_prot = prot_to_mmu(rgn->prot);
            const auto request = build_contract_request(
                space, rgn->va_start, rgn->size(), new_prot, rgn->flags,
                rgn->backing, rgn->phys_base, rgn->storage_key(), rgn->numa_node());
            auto protect_result = zxfoundation::dgp::memory_contract::authorize_protect(
                request, old_prot);
            if (!protect_result) {
                sys::syschk::syschk_fatal(protect_result.error(),
                    "vm: prevalidated protection authorization changed during commit");
            }
            // Update VMA protection.
            auto policy_result = commit_region_policy(
                space, *rgn, *protect_result);
            if (!policy_result) {
                sys::syschk::syschk_fatal(policy_result.error(),
                    "vm: prevalidated storage policy disappeared during protect commit");
            }
            rgn->prot = new_prot;

            for (u64 cursor = rgn->va_start; cursor < rgn->va_end; cursor += PAGE_SIZE) {
                auto pte_result = protect<dat_level::region_1>(root, cursor, hw_prot);
                if (!pte_result &&
                    pte_result.error() != lib::kernel_error::from_memory(
                        lib::memory_error::invalid_address)) {
                    sys::syschk::syschk_fatal(pte_result.error(),
                        "vm: unexpected PTE protection failure after VMA commit");
                }
            }

            const u8 region_key = rgn->storage_key();
            const bool new_fp = (new_prot & vm_prot::write) == vm_prot::none;
            if (rgn->pages.slots) {
                for (u32 i = 0U; i < rgn->pages.count; ++i) {
                    const auto& slot = rgn->pages.slots[i];
                    if (slot.state == vm_page_state::empty) continue;
                    if (slot.state == vm_page_state::portal) continue;
                    if (slot.phys == 0ULL) continue;
                    arch::s390x::cpu::skey::sske_preserve_rc(
                        slot.phys, region_key, new_fp);
                }
            }

            apply_vm_invalidation(root_phys, *rgn, protect_result->invalidate);

            node = lib::rb_next(node);
        }

        return {};
    }


    /// @brief Destroy all regions in a VME space, freeing region descriptors.
    auto vm_destroy_space(vm_space& space) noexcept -> void {
        using namespace detail;

        const sync::irq_lock_guard guard{space.lock};

        lib::rb_node* cleanup_node = space.tree.leftmost;
        while (cleanup_node) {
            auto* cleanup_region = region_of(cleanup_node);
            cleanup_node = lib::rb_next(cleanup_node);

            if (cleanup_region->pages.slots) {
                detail::unmap_range_pages(
                    space.asce_val & arch::s390x::mmu::ASCE_ORIGIN_MASK,
                    cleanup_region->va_start, cleanup_region->va_end);
            } else if (cleanup_region->backing == vm_backing::physical
                       && cleanup_region->phys_base != 0ULL) {
                detail::unmap_range_pages(
                    space.asce_val & arch::s390x::mmu::ASCE_ORIGIN_MASK,
                    cleanup_region->va_start,
                    cleanup_region->va_end);
            }
        }

        if (space.asce_val != 0U) {
            arch::s390x::mmu::tlb_flush_asce(space.asce_val);
        }

        for (lib::rb_node* cleanup_node = space.tree.leftmost;
             cleanup_node; cleanup_node = lib::rb_next(cleanup_node)) {
            auto* cleanup_region = region_of(cleanup_node);
            discard_value detail::teardown_region_slots(*cleanup_region);
        }

        lib::rb_node* node = space.tree.leftmost;
        while (node) {
            auto* rgn = region_of(node);
            lib::rb_node* next = lib::rb_next(node);
            lib::rb_erase_cached(node, space.tree);
            destroy_region_memory_objects(*rgn);
            discard_value scoms::object_core::detach(rgn->ko);
            discard_value g_region_table.release(rgn->ko.id);
            node = next;
        }

        // Reset accounting.
        space.tree           = {};
        space.total_mapped    = 0;
        space.total_committed = 0;
        space.total_resident  = 0;
        space.region_count    = 0;
        space.owner_object    = nullptr;
    }

    /// @brief Look up a vm_region by SCOMS ID.
    /// @return Raw pointer, or nullptr if not found.
    [[nodiscard]] auto vm_region_find_by_id(u32 id) noexcept -> vm_region* {
        return detail::g_region_table.find_raw(id);
    }

    /// @brief Initialize the VME subsystem (create SCOMS table for vm_region).
    auto vm_init() noexcept -> void {
        using namespace detail;

        if (g_vme_initialized) return;

        g_region_table.init(scoms::kobject::kobject_type::vm_region);
        g_vme_initialized = true;

        detail::register_vm_cap_lookup();
    }

} // namespace zxfoundation::memory::vm

