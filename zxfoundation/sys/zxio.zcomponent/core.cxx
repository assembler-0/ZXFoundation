/// SPDX-License-Identifier: Apache-2.0
/// @file io.cxxm
/// @brief Capability-bound CSS I/O, DMA authority, and completion delivery.

module zxfoundation.sys.zxio.core;
import zxfoundation.sys.zxio.types;
import zxfoundation.base.types;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.table;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.scoms.metadata.core;
import zxfoundation.sys.cap.types;
import zxfoundation.sys.cap.core;
import zxfoundation.sys.printk.core;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm_types;
import zxfoundation.memory.hhdm;
import zxfoundation.memory.memory_objects;
import zxfoundation.dgp.isolation_types;
import zxfoundation.dgp.memory_contract;
import zxfoundation.domain.domain;
import zxfoundation.domain.domain_types;
import zxfoundation.domain.signal;
import zxfoundation.domain.signal_types;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.base.typestate;
import zxfoundation.base.transaction;
import arch.s390x.io.types;
import arch.s390x.io.css;
import arch.s390x.cpu.skey;
import arch.s390x.cpu.sync;
import lib.error;
import std;
import lib.format;

namespace zxfoundation::sys::zxio::detail {

    using namespace zxfoundation::memory;
    using namespace arch::s390x::io;
    using namespace scoms;
    using namespace cap;

    no_export scoms_table<io_binding, MAX_IO_BINDINGS> g_io_table{};
    no_export bool g_initialized{false};

    no_export constexpr u32 DMA_ORDER = 2U;
    no_export constexpr u64 DMA_BYTES = PAGE_SIZE << DMA_ORDER;
    no_export constexpr u32 CCW_STAGE_OFFSET = 0U;
    no_export constexpr u32 DRIVER_DMA_OFFSET = PAGE_SIZE;
    no_export constexpr u64 DRIVER_DMA_BYTES = DMA_BYTES - DRIVER_DMA_OFFSET;
    no_export constexpr u32 IO_FENCE_POLL_LIMIT = 65'536U;
    no_export constexpr u8 CCW_USER_FLAGS = CCW_FLAG_CD | CCW_FLAG_CC | CCW_FLAG_SLI |
        CCW_FLAG_SKIP | CCW_FLAG_PCI | CCW_FLAG_SUSPEND;

    [[nodiscard]] no_export auto lookup_binding(
        u32 id, u32 generation
    ) noexcept -> kobject::robject<io_binding, lifecycle_state::active> {
        auto ref = g_io_table.acquire_ref<lifecycle_state::active>(id);
        if (!ref || ref->ko.generation != generation) return {};
        return ref;
    }

    [[nodiscard]] no_export auto binding_for_owner(
        cap_token token, cap_rights rights, u32 owner_domain
    ) noexcept -> std::expected<io_binding*, lib::kernel_error> {
        if (!token || token.type() != cap_type::io_device) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        auto owner = cap_validate_generic_for(token, rights, owner_domain);
        if (!owner) return std::unexpected(owner.error());
        auto resolved = cap_resolve(token, rights);
        if (!resolved) return std::unexpected(resolved.error());
        auto* binding = reinterpret_cast<io_binding*>(*resolved);
        if (binding->owner_domain != owner_domain ||
            binding->ko.state != lifecycle_state::active) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        return binding;
    }

    [[nodiscard]] no_export auto validate_dma_authority(
        const io_binding& binding
    ) noexcept -> std::expected<objects::memory_object*, lib::kernel_error> {
        auto storage_result = objects::memory_object_resolve(binding.dma_storage);
        if (!storage_result) return std::unexpected(storage_result.error());
        auto* storage = *storage_result;
        if (storage->owner_domain != binding.owner_domain ||
            storage->storage_key != binding.storage_key ||
            storage->storage_key_generation != binding.storage_key_generation ||
            storage->range_start != binding.dma_base ||
            storage->range_length != binding.dma_size) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        auto extent_result = objects::memory_object_resolve(binding.dma_extent);
        if (!extent_result || (*extent_result)->owner_domain != binding.owner_domain ||
            (*extent_result)->range_start != binding.dma_base + DRIVER_DMA_OFFSET ||
            (*extent_result)->range_length != DRIVER_DMA_BYTES ||
            (*extent_result)->storage_key != binding.storage_key ||
            (*extent_result)->storage_key_generation != binding.storage_key_generation) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        auto frame_result = pmm::frame_validate_mapping(
            (*extent_result)->range_start, (*extent_result)->range_length,
            binding.dma_extent.id, binding.dma_extent.generation);
        if (!frame_result) return std::unexpected(frame_result.error());
        return storage;
    }

    auto clear_dma_frame_authority(u64 phys_base, u64 size) noexcept -> void {
        for (u64 phys = phys_base; phys < phys_base + size; phys += PAGE_SIZE) {
            discard_value pmm::frame_clear_mapping_metadata(phys);
            arch::s390x::cpu::skey::sske(phys,
                arch::s390x::cpu::skey::make_skey(0U, false));
        }
    }

    [[nodiscard]] no_export auto verify_and_stage_ccws(
        const io_binding& binding, const io_request& request
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (request.request_id == 0U || request.ccw_count == 0U ||
            request.ccw_count > IO_MAX_CCWS || request.flags != 0U ||
            (request.ccw_offset & (alignof(ccw1) - 1U)) != 0U) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }

        const u64 chain_bytes = static_cast<u64>(request.ccw_count) * sizeof(ccw1);
        if (request.ccw_offset + chain_bytes < request.ccw_offset ||
            request.ccw_offset + chain_bytes > DRIVER_DMA_BYTES) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::invalid_address));
        }

        const auto* source = reinterpret_cast<const ccw1*>(
            hhdm::phys_to_virt(
                binding.dma_base + DRIVER_DMA_OFFSET + request.ccw_offset));
        auto* staged = reinterpret_cast<ccw1*>(
            hhdm::phys_to_virt(binding.dma_base + CCW_STAGE_OFFSET));

        for (u16 i = 0U; i < request.ccw_count; ++i) {
            const ccw1 ccw = source[i];
            if ((ccw.flags & ~CCW_USER_FLAGS) != 0U ||
                (ccw.flags & (CCW_FLAG_IDA | CCW_FLAG_MIDA)) != 0U ||
                ccw.cmd == CCW_CMD_TIC) {
                return std::unexpected(lib::kernel_error::from_memory(
                    lib::memory_error::capability_invalid));
            }

            const bool transfers_data = ccw.count != 0U &&
                (ccw.flags & CCW_FLAG_SKIP) == 0U && ccw.cmd != CCW_CMD_NOP;
            if (transfers_data) {
                if (ccw.data_addr + ccw.count < ccw.data_addr ||
                    static_cast<u64>(ccw.data_addr) + ccw.count > DRIVER_DMA_BYTES) {
                    return std::unexpected(lib::kernel_error::from_memory(
                        lib::memory_error::invalid_address));
                }
            } else if (ccw.data_addr != 0U) {
                return std::unexpected(lib::kernel_error::from_generic(
                    lib::generic_error::invalid_arg));
            }

            staged[i] = ccw1{
                .cmd = ccw.cmd,
                .flags = ccw.flags,
                .count = ccw.count,
                .data_addr = transfers_data
                    ? static_cast<u32>(binding.dma_base + DRIVER_DMA_OFFSET + ccw.data_addr)
                    : 0U,
            };
        }
        return {};
    }

    [[nodiscard]] no_export auto enqueue_completion(
        io_binding& binding, const irb& status
    ) noexcept -> bool {
        const sync::irq_lock_guard guard{binding.ko.lock};
        if (binding.ko.state != lifecycle_state::active ||
            binding.pending_request_id == 0U ||
            (binding.op_state != io_substate::pending &&
             binding.op_state != io_substate::revoking)) return false;

        io_completion completion{
            .request_id = binding.pending_request_id,
            .residual_count = status.scsw_data.count,
            .device_status = status.scsw_data.dev_status,
            .subchannel_status = status.scsw_data.sch_status,
            .flags = status.scsw_data.has_error() ? u16{1} : u16{0},
        };
        if (binding.completion_count == IO_COMPLETION_DEPTH) {
            binding.completion_head =
                (binding.completion_head + 1U) % IO_COMPLETION_DEPTH;
            --binding.completion_count;
            ++binding.completion_dropped;
        }
        binding.completions[binding.completion_tail] = completion;
        binding.completion_tail =
            (binding.completion_tail + 1U) % IO_COMPLETION_DEPTH;
        ++binding.completion_count;
        binding.pending_request_id = 0U;
        if (!binding.teardown_started) {
            binding.op_state = status.scsw_data.has_error()
                ? io_substate::error : io_substate::bound;
        }
        return true;
    }

    no_export auto css_completion_sink(
        u32 sch_index, u32 binding_id, u32 binding_generation,
        const irb& status
    ) noexcept -> void {
        discard_value sch_index;
        auto binding = lookup_binding(binding_id, binding_generation);
        if (!binding) return;
        const u32 owner = binding->owner_domain;
        if (!enqueue_completion(*binding, status)) return;
        if (auto* domain = zxfoundation::domain::domain_find(owner)) {
            zxfoundation::domain::signal::signal_enqueue(
                *domain, zxfoundation::domain::signal::signal_kind::io,
                binding_id);
        }
    }

    [[nodiscard]] no_export auto fence_binding_io(io_binding& binding) noexcept
        -> std::expected<void, lib::kernel_error> {
        {
            const sync::irq_lock_guard guard{binding.ko.lock};
            if (binding.pending_request_id == 0U) return {};
        }

        auto halt = css::halt_io(binding.sch_index);
        if (!halt && halt.error().category != lib::error_category::driver) {
            return std::unexpected(halt.error());
        }
        auto clear = css::clear_io(binding.sch_index);
        if (!clear && clear.error().category != lib::error_category::driver) {
            return std::unexpected(clear.error());
        }

        for (u32 attempt = 0U; attempt < IO_FENCE_POLL_LIMIT; ++attempt) {
            irb status{};
            auto test = css::test_io(binding.sch_index, status);
            if (!test) return std::unexpected(test.error());
            if (*test == 0U &&
                (status.scsw_data.actl() & (ACTL_DEV_ACTIVE | ACTL_SCH_ACTIVE)) == 0U) {
                discard_value enqueue_completion(binding, status);
                return {};
            }
            if (auto* entry = css::get_entry(binding.sch_index);
                entry && entry->state == css::subchannel_state::idle) {
                return {};
            }
            arch::s390x::cpu::sync::cpu_relax();
        }
        return std::unexpected(lib::kernel_error::from_driver(
            lib::driver_error::timeout));
    }

    class io_bind_txn : public zxfoundation::base::transaction<io_bind_txn> {
        pmm::folio_embryo m_dma{};
        bool m_dma_valid{false};
        objects::memory_object_ref m_storage{};
        objects::memory_object_ref m_extent{};
        kobject::robject<io_binding, lifecycle_state::embryo> m_ref{};
        bool m_ref_valid{false};
        bool m_css_claimed{false};

    public:
        auto take_dma(pmm::folio_embryo&& folio) noexcept -> void {
            m_dma = std::move(folio);
            m_dma_valid = true;
        }
        auto take_ref(kobject::robject<io_binding, lifecycle_state::embryo>&& ref) noexcept -> void {
            m_ref = std::move(ref);
            m_ref_valid = true;
        }
        auto take_storage(objects::memory_object_ref ref) noexcept -> void { m_storage = ref; }
        auto take_extent(objects::memory_object_ref ref) noexcept -> void { m_extent = ref; }
        auto mark_css_claimed() noexcept -> void { m_css_claimed = true; }
        [[nodiscard]] auto dma_phys() const noexcept -> u64 { return m_dma.phys_base; }
        [[nodiscard]] auto dma_size() const noexcept -> u64 { return m_dma.size_bytes(); }
        [[nodiscard]] auto binding() noexcept -> io_binding& { return *m_ref; }
        [[nodiscard]] auto activate_binding() noexcept
            -> std::expected<void, lib::kernel_error> {
            if (!m_ref_valid || !m_ref->ko.transition(lifecycle_state::active)) {
                return std::unexpected(lib::kernel_error::from_generic(
                    lib::generic_error::invalid_arg));
            }
            return {};
        }

        auto on_rollback() noexcept -> void {
            if (m_css_claimed && m_ref_valid) {
                discard_value css::release_binding(
                    m_ref->sch_index, m_ref->ko.id, m_ref->ko.generation);
            }
            if (m_extent) discard_value objects::memory_object_destroy(m_extent);
            if (m_storage) discard_value objects::memory_object_destroy(m_storage);
            if (m_ref_valid) discard_value g_io_table.release(m_ref.id());
            if (m_dma_valid) {
                clear_dma_frame_authority(m_dma.phys_base, m_dma.size_bytes());
                pmm::free(std::move(m_dma).make_active());
            }
        }
    };

} // namespace zxfoundation::sys::zxio::detail

namespace zxfoundation::sys::zxio {

    using namespace cap;

    auto init() noexcept -> void {
        using namespace detail;
        if (g_initialized) return;
        g_io_table.init(scoms::kobject::kobject_type::io_binding);
        auto sink_result = css::install_completion_sink(css_completion_sink);
        if (!sink_result) __builtin_trap();
        discard_value register_lookup(cap_type::io_device,
            [](u16 obj_id) noexcept -> scoms::kobject::bobject* {
                auto* binding = binding_find(static_cast<u32>(obj_id));
                return binding ? &binding->ko : nullptr;
            });
        g_initialized = true;
        sys::printk::pinfo("io: capability-bound CSS/DMA subsystem initialized");
    }

    [[nodiscard]] auto bind(
        u16 dev_no, u32 domain_id, cap_token& out_token
    ) noexcept -> std::expected<void, lib::kernel_error> {
        using namespace detail;
        auto* domain = zxfoundation::domain::domain_find(domain_id);
        if (!domain || domain->ko.state != lifecycle_state::active) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::not_found));
        }

        u16 sch_index = 0xFFFFU;
        for (u32 i = 0U; i < css::subchannel_count(); ++i) {
            auto* entry = css::get_entry(i);
            if (entry && entry->devno == dev_no) {
                sch_index = static_cast<u16>(i);
                break;
            }
        }
        if (sch_index == 0xFFFFU) {
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::not_found));
        }

        io_bind_txn txn{};
        auto dma_result = pmm::alloc(DMA_ORDER, pmm::zone::dma31,
            pmm::purpose::device_dma, pmm::page_init::zeroed);
        if (!dma_result) return std::unexpected(dma_result.error());
        txn.take_dma(std::move(*dma_result));

        auto binding_result = g_io_table.alloc();
        if (!binding_result) return std::unexpected(binding_result.error());
        txn.take_ref(std::move(*binding_result));
        auto& binding = txn.binding();
        binding.dev_no = dev_no;
        binding.sch_index = sch_index;
        binding.owner_domain = domain_id;
        binding.dma_base = txn.dma_phys();
        binding.dma_size = txn.dma_size();
        binding.storage_key = domain->storage_key;
        binding.storage_key_generation = domain->storage_key_generation;
        binding.allowed_lpm = css::available_lpm(sch_index);
        if (binding.allowed_lpm == 0U) {
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::not_ready));
        }

        auto owner_result = scoms::object_core::set_owner(binding.ko, domain_id);
        if (!owner_result) return std::unexpected(owner_result.error());
        auto attach_result = scoms::object_core::attach(binding.ko, domain->ko);
        if (!attach_result) return std::unexpected(attach_result.error());

        zxfoundation::memory::objects::memory_object_policy policy{
            .storage_key = binding.storage_key,
            .storage_key_generation = binding.storage_key_generation,
            .numa_node = domain->numa_node,
            .dgp_scope = dgp::memory_scope::private_domain,
            .dgp_access = dgp::memory_access::read | dgp::memory_access::write,
            .dgp_rights = dgp::isolation_rights::map,
            .invalidate = dgp::invalidation_scope::address_space,
            .mapping_generation = 1U,
        };
        auto storage = zxfoundation::memory::objects::memory_object_create(
            scoms::kobject::kobject_type::storage, domain_id, &binding.ko,
            binding.dma_base, binding.dma_size, dgp::isolation_rights::map, policy);
        if (!storage) return std::unexpected(storage.error());
        binding.dma_storage = zxfoundation::memory::objects::memory_object_ref_of(**storage);
        txn.take_storage(binding.dma_storage);

        auto extent = zxfoundation::memory::objects::memory_object_create(
            scoms::kobject::kobject_type::memory_extent, domain_id, &(*storage)->ko,
            binding.dma_base + DRIVER_DMA_OFFSET, DRIVER_DMA_BYTES,
            dgp::isolation_rights::map, policy);
        if (!extent) return std::unexpected(extent.error());
        binding.dma_extent = zxfoundation::memory::objects::memory_object_ref_of(**extent);
        txn.take_extent(binding.dma_extent);

        for (u64 phys = binding.dma_base; phys < binding.dma_base + binding.dma_size;
             phys += PAGE_SIZE) {
            const auto authority = phys < binding.dma_base + DRIVER_DMA_OFFSET
                ? binding.dma_storage : binding.dma_extent;
            auto metadata = pmm::frame_set_mapping_metadata(
                phys, authority.id, authority.generation, true);
            if (!metadata) return std::unexpected(metadata.error());
            arch::s390x::cpu::skey::sske(phys,
                arch::s390x::cpu::skey::make_skey(binding.storage_key, false));
        }

        auto claim = css::claim_binding(
            binding.sch_index, binding.ko.id, binding.ko.generation);
        if (!claim) return std::unexpected(claim.error());
        txn.mark_css_claimed();

        auto activate = txn.activate_binding();
        if (!activate) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::invalid_arg));

        auto token = cap_issue(cap_type::io_device,
            static_cast<u16>(binding.ko.id),
            cap_rights::read | cap_rights::write | cap_rights::exec | cap_rights::destroy,
            domain_id);
        if (!token) return std::unexpected(token.error());
        out_token = *token;
        txn.commit();
        sys::printk::pinfo("io: bound dev_no={:#06x} domain={} binding={} dma={:#x}",
            dev_no, domain_id, binding.ko.id, binding.dma_base);
        return {};
    }

    [[nodiscard]] auto unbind(
        cap_token token, u32 owner_domain
    ) noexcept -> std::expected<void, lib::kernel_error> {
        using namespace detail;
        auto binding_result = binding_for_owner(token, cap_rights::destroy, owner_domain);
        if (!binding_result) return std::unexpected(binding_result.error());
        auto* binding = *binding_result;
        const u16 dev_no = binding->dev_no;
        const u32 binding_id = binding->ko.id;
        const u32 binding_generation = binding->ko.generation;
        bool resume_teardown{false};
        io_substate prior_state{};

        {
            const sync::irq_lock_guard guard{binding->ko.lock};
            if (binding->teardown_started) {
                if (binding->op_state != io_substate::revoking) {
                    return std::unexpected(lib::kernel_error::from_driver(
                        lib::driver_error::busy));
                }
                resume_teardown = true;
            } else {
                prior_state = binding->op_state;
                binding->teardown_started = true;
                binding->op_state = io_substate::revoking;
            }
        }

        if (!resume_teardown) {
            auto retire = zxfoundation::memory::objects::memory_extent_retire(binding->dma_extent);
            if (!retire) {
                const sync::irq_lock_guard guard{binding->ko.lock};
                binding->teardown_started = false;
                binding->op_state = prior_state;
                return std::unexpected(retire.error());
            }
        }
        auto fence = fence_binding_io(*binding);
        if (!fence) return std::unexpected(fence.error());
        auto release_css = css::release_binding(
            binding->sch_index, binding_id, binding_generation);
        if (!release_css) return std::unexpected(release_css.error());

        auto revoke_binding = cap_revoke_target(binding->ko);
        if (!revoke_binding) return std::unexpected(revoke_binding.error());
        auto extent_result = zxfoundation::memory::objects::memory_object_resolve(binding->dma_extent);
        if (!extent_result) return std::unexpected(extent_result.error());
        auto revoke_extent = cap_revoke_target(
            (*extent_result)->ko);
        if (!revoke_extent) return std::unexpected(revoke_extent.error());
        auto destroy_extent = zxfoundation::memory::objects::memory_object_destroy(binding->dma_extent);
        if (!destroy_extent) return std::unexpected(destroy_extent.error());
        auto destroy_storage = zxfoundation::memory::objects::memory_object_destroy(binding->dma_storage);
        if (!destroy_storage) return std::unexpected(destroy_storage.error());

        clear_dma_frame_authority(binding->dma_base, binding->dma_size);
        pmm::free(pmm::folio_active{
            binding->dma_base, DMA_ORDER, pmm::purpose::device_dma, pmm::zone::dma31});
        auto release = g_io_table.release(binding_id);
        if (!release) return std::unexpected(release.error());
        sys::printk::pinfo("io: unbound dev_no={:#06x}", dev_no);
        return {};
    }

    [[nodiscard]] auto dma_extent(
        cap_token token, u32 owner_domain
    ) noexcept -> std::expected<cap_token, lib::kernel_error> {
        auto binding_result = detail::binding_for_owner(
            token, cap_rights::read, owner_domain);
        if (!binding_result) return std::unexpected(binding_result.error());
        {
            const sync::irq_lock_guard guard{(*binding_result)->ko.lock};
            if ((*binding_result)->teardown_started) {
                return std::unexpected(lib::kernel_error::from_driver(
                    lib::driver_error::busy));
            }
        }
        auto authority = detail::validate_dma_authority(**binding_result);
        if (!authority) return std::unexpected(authority.error());
        auto extent = zxfoundation::memory::objects::memory_object_resolve(
            (*binding_result)->dma_extent);
        if (!extent || (*extent)->owner_domain != owner_domain ||
            (*extent)->storage_key_generation != (*binding_result)->storage_key_generation) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }
        return cap_issue(cap_type::memory_extent, static_cast<u16>((*extent)->ko.id),
            cap_rights::read | cap_rights::write | cap_rights::map, owner_domain);
    }

    [[nodiscard]] auto submit(
        cap_token token, u32 owner_domain, const io_request& request
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto binding_result = detail::binding_for_owner(
            token, cap_rights::exec, owner_domain);
        if (!binding_result) return std::unexpected(binding_result.error());
        auto* binding = *binding_result;
        auto authority = detail::validate_dma_authority(*binding);
        if (!authority) return std::unexpected(authority.error());

        arch::s390x::io::orb kernel_orb{};
        {
            // Serializing staging and SSCH against unbind closes the interval
            // where a teardown could clear the subchannel before it is started.
            const sync::irq_lock_guard guard{binding->ko.lock};
            if (binding->teardown_started || binding->op_state != io_substate::bound) {
                return std::unexpected(lib::kernel_error::from_driver(
                    lib::driver_error::busy));
            }
            auto staged = detail::verify_and_stage_ccws(*binding, request);
            if (!staged) return std::unexpected(staged.error());

            binding->op_state = io_substate::pending;
            binding->pending_request_id = request.request_id;
            kernel_orb.intparm = binding->ko.id;
            kernel_orb.set_format1();
            kernel_orb.set_key(binding->storage_key);
            kernel_orb.set_cpa(static_cast<u32>(binding->dma_base + detail::CCW_STAGE_OFFSET));
            kernel_orb.set_lpm(binding->allowed_lpm);
            auto start = arch::s390x::io::css::start_io(binding->sch_index, kernel_orb);
            if (!start) {
                binding->op_state = io_substate::bound;
                binding->pending_request_id = 0U;
                return std::unexpected(start.error());
            }
        }
        return {};
    }

    [[nodiscard]] auto completion_dequeue(
        cap_token token, u32 owner_domain
    ) noexcept -> std::expected<io_completion, lib::kernel_error> {
        auto binding_result = detail::binding_for_owner(
            token, cap_rights::read, owner_domain);
        if (!binding_result) return std::unexpected(binding_result.error());
        auto* binding = *binding_result;
        const sync::irq_lock_guard guard{binding->ko.lock};
        if (binding->completion_count == 0U) {
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::not_ready));
        }
        const io_completion result = binding->completions[binding->completion_head];
        binding->completion_head =
            (binding->completion_head + 1U) % IO_COMPLETION_DEPTH;
        --binding->completion_count;
        return result;
    }

    [[nodiscard]] auto irq_bind(
        cap_token token, u32 owner_domain, u32 retired_gate_id
    ) noexcept -> std::expected<void, lib::kernel_error> {
        discard_value token;
        discard_value owner_domain;
        discard_value retired_gate_id;
        return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::not_supported));
    }

    auto destroy_for_domain(u32 domain_id) noexcept -> void {
        for (u32 id = 0U; id < MAX_IO_BINDINGS; ++id) {
            auto* binding = binding_find(id);
            if (!binding || binding->owner_domain != domain_id) continue;
            auto token = cap_issue(cap_type::io_device, static_cast<u16>(id),
                cap_rights::destroy, domain_id);
            if (token) discard_value unbind(*token, domain_id);
        }
    }

    auto binding_count() noexcept -> u32 { return detail::g_io_table.count(); }
    auto binding_find(u32 id) noexcept -> io_binding* {
        return detail::g_io_table.find_raw(id);
    }

} // namespace zxfoundation::sys::zxio
