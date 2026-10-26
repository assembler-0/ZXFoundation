/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/memory/slub.zcomponent/core.cxx
/// @brief SLUB slab allocator — full SMP-aware implementation — implementation.

module zxfoundation.memory.slub.core;
import zxfoundation.memory.slub.types;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.hhdm;
import zxfoundation.memory.objects;
import zxfoundation.memory.numa;
import zxfoundation.memory.numa.types;
import zxfoundation.sync.rcu.core;
import zxfoundation.sync.rcu.types;
import zxfoundation.sched.context.core;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sync.lockable;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.zinit.core;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.kobject.types;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.clock;
import lib.error;
import lib.align;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace zxfoundation::memory::slub::detail {

    using namespace pmm;

    /// @brief Dynamically allocated pool of kmem_cache objects
    static kmem_cache* g_cache_table_ptr{nullptr};
    static u32         g_cache_count{0};
    alignas(4) static sync::qspinlock::qspinlock<sync::lock_rank::slub_cache> g_cache_table_lock{};
    static bool        g_initialized{false};

    [[nodiscard]] inline auto encode_fp(
        const kmem_cache& cache,
        void* obj,
        void* next
    ) noexcept -> void* {
        return reinterpret_cast<void*>(
            reinterpret_cast<u64>(next)
            ^ cache.random_seq
            ^ reinterpret_cast<u64>(
                static_cast<u8*>(obj) + cache.fp_offset));
    }

    [[nodiscard]] inline auto decode_fp(
        const kmem_cache& cache,
        void* obj,
        void* stored
    ) noexcept -> void* {
        return reinterpret_cast<void*>(
            reinterpret_cast<u64>(stored)
            ^ cache.random_seq
            ^ reinterpret_cast<u64>(
                static_cast<u8*>(obj) + cache.fp_offset));
    }

    /// @brief Write the encoded next-pointer into a free object.
    inline auto set_fp(
        const kmem_cache& cache,
        void* obj,
        void* next
    ) noexcept -> void {
        void** slot = reinterpret_cast<void**>(
            static_cast<u8*>(obj) + cache.fp_offset);
        *slot = encode_fp(cache, obj, next);
    }

    /// @brief Read and decode the next-pointer from a free object.
    [[nodiscard]] inline auto get_fp(
        const kmem_cache& cache,
        void* obj
    ) noexcept -> void* {
        void** slot = reinterpret_cast<void**>(
            static_cast<u8*>(obj) + cache.fp_offset);
        return decode_fp(cache, obj, *slot);
    }

    [[nodiscard]] inline auto node_state(
        kmem_cache& cache, u8 node_id
    ) noexcept -> kmem_cache_node& {
        return cache.node[node_id < numa::MAX_NUMA_NODES
            ? node_id : numa::NUMA_NODE_BOOT];
    }

    /// @brief Recover the slab header for a cache-known allocation.
    [[nodiscard]] inline auto slab_of(
        const kmem_cache& cache,
        void* obj
    ) noexcept -> slab_header* {
        const u64 phys    = hhdm::virt_to_phys(reinterpret_cast<u64>(obj));
        const u64 slabmsk = ~((static_cast<u64>(PAGE_SIZE)
            << cache.order) - 1ULL);
        const u64 sphys   = phys & slabmsk;
        return reinterpret_cast<slab_header*>(hhdm::phys_to_virt(sphys));
    }

    /// @brief Compute the first object base address within a slab.
    [[nodiscard]] inline auto first_object(
        const kmem_cache& cache,
        slab_header* sh
    ) noexcept -> void* {
        const u64 base     = reinterpret_cast<u64>(sh);
        const u64 hdr_end  = base + sizeof(slab_header);
        const u64 aligned  = lib::align_up(hdr_end, static_cast<u64>(cache.align));
        return reinterpret_cast<void*>(aligned);
    }

    /// @brief RCU callback: free a slab page to PMM after grace period.
    auto free_slab_rcu_cb(sync::rcu::rcu_head* head) noexcept -> void {
        const u64 rcu_off = __builtin_offsetof(slab_header, rcu);
        auto* sh = reinterpret_cast<slab_header*>(
            reinterpret_cast<u8*>(head) - rcu_off);
        pmm::free({ sh->phys, sh->rcu_order, purpose::slab, zone::normal });
    }

    /// @brief Free a slab page: immediately or via RCU depending on flags.
    auto free_slab(kmem_cache& cache, slab_header* sh) noexcept -> void {
        discard_value cache.stat_free_slab.fetch_add(1U, memory_order::relaxed);
        auto& node = node_state(cache, sh->node_id);
        discard_value node.total_slabs.fetch_sub(1U, memory_order::relaxed);
        discard_value node.total_objects.fetch_sub(sh->objects, memory_order::relaxed);

        const bool use_rcu = !!(cache.flags & slub_flags::typesafe_by_rcu);
        if (use_rcu) [[unlikely]] {
            sh->rcu_order = cache.order;
            sync::rcu::call_rcu(&sh->rcu, free_slab_rcu_cb);
        } else {
            pmm::free({ sh->phys, cache.order, purpose::slab, zone::normal });
        }
    }

    /// @brief Allocate a new slab page from PMM and initialise it.
    /// @return Pointer to the initialised slab_header, or nullptr on OOM.
    [[nodiscard]] auto alloc_new_slab(
        kmem_cache& cache, u8 preferred_node
    ) noexcept -> slab_header* {
        auto result = pmm::alloc(pmm::allocation_request{
            .order = cache.order,
            .mem_zone = zone::normal,
            .use = purpose::slab,
            .init = page_init::uninitialized,
            .placement = numa::allocation_policy{
                .preferred_node = preferred_node,
                .fallback = numa::fallback_mode::local_first,
                .require_dma31 = false,
                .allow_remote = true,
            },
        });
        if (!result) [[unlikely]] return nullptr;

        const u64 phys    = result->phys_base;
        auto* sh = reinterpret_cast<slab_header*>(hhdm::phys_to_virt(phys));

        sh->next             = nullptr;
        sh->prev             = nullptr;
        sh->cpu_partial_next = nullptr;
        sh->phys             = phys;
        sh->cache            = &cache;   // back-pointer: stable (lives in g_cache_table_ptr[])
        sh->slab_order       = cache.order; // set once at alloc, used by kmem_free_raw fast path
        sh->frozen           = 1;
        sh->node_id          = result->placement.selected_node;
        sh->on_node_partial  = 0;
        sh->inuse.store(0U, memory_order::relaxed);
        sh->objects          = cache.objects_per_slab;
        sh->rcu_order        = cache.order; // pre-initialised; overwritten in free_slab if RCU
        sh->rcu              = {};
        sh->_pad             = 0U;

        void* first = first_object(cache, sh);
        void* obj   = first;
        for (u32 i = 0; i < cache.objects_per_slab - 1U; ++i) {
            void* next = static_cast<u8*>(obj) + cache.size;
            set_fp(cache, obj, next);
            obj = next;
        }
        set_fp(cache, obj, nullptr);
        sh->freelist = first;

        discard_value cache.stat_alloc_slab.fetch_add(1U, memory_order::relaxed);
        auto& node = node_state(cache, sh->node_id);
        discard_value node.total_slabs.fetch_add(1U, memory_order::relaxed);
        discard_value node.total_objects.fetch_add(sh->objects, memory_order::relaxed);

        return sh;
    }

    /// @brief Remove a slab from the node partial list.
    /// @pre  The slab owner's node lock is held.
    inline auto node_remove_partial(kmem_cache& cache, slab_header* sh) noexcept -> void {
        auto& node = node_state(cache, sh->node_id);
        if (!sh->on_node_partial || node.nr_partial == 0U) __builtin_trap();
        if (sh->prev) sh->prev->next = sh->next;
        else          node.partial = sh->next;
        if (sh->next) sh->next->prev = sh->prev;
        sh->next = nullptr;
        sh->prev = nullptr;
        sh->on_node_partial = 0U;
        --node.nr_partial;
    }

    /// @brief Add a slab to the node partial list (prepend).
    /// @pre  The slab owner's node lock is held.
    inline auto node_add_partial(kmem_cache& cache, slab_header* sh) noexcept -> void {
        auto& node = node_state(cache, sh->node_id);
        if (sh->on_node_partial) __builtin_trap();
        sh->next = node.partial;
        sh->prev = nullptr;
        if (node.partial) node.partial->prev = sh;
        node.partial = sh;
        sh->on_node_partial = 1U;
        ++node.nr_partial;
    }

    /// @brief Activate a slab: freeze it and set as the CPU's active slab.
    /// @pre  preempt_disable + irq_save_disable in effect.
    inline auto activate_slab(
        [[maybe_unused]] kmem_cache& cache,
        kmem_cache_cpu&   cs,
        slab_header*      sh
    ) noexcept -> void {
        if (sh->on_node_partial) __builtin_trap();
        sh->frozen  = 1U;
        cs.slab     = sh;
        cs.freelist = sh->freelist;
        sh->freelist = nullptr; // Now owned by cpu_slab.
    }

    /// @brief Return a CPU-owned slab to its PMM-owner partial list.
    /// The owner-node lock serializes concurrent remote frees.
    auto deactivate_cpu_slab(kmem_cache& cache, kmem_cache_cpu& cs) noexcept -> void {
        auto* sh = cs.slab;
        if (!sh) return;
        auto& node = node_state(cache, sh->node_id);
        const sync::irq_lock_guard _{node.list_lock};
        if (cs.freelist) {
            void* tail = cs.freelist;
            while (void* next = get_fp(cache, tail)) tail = next;
            set_fp(cache, tail, sh->freelist);
            sh->freelist = cs.freelist;
        }
        cs.freelist = nullptr;
        cs.slab = nullptr;
        sh->frozen = 0U;
        if (sh->inuse.load(memory_order::acquire) < sh->objects &&
            !sh->on_node_partial) {
            node_add_partial(cache, sh);
        }
    }

    [[nodiscard]] auto alloc_slow(kmem_cache& cache) noexcept -> void*;
    auto free_slow(kmem_cache& cache, slab_header* sh, void* obj) noexcept -> void;

    [[nodiscard]] auto alloc_slow(kmem_cache& cache) noexcept -> void* {
        sched::context::preempt_disable();
        const auto setup_flags = arch::s390x::cpu::irq::irq_save_disable();
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto& cs  = cache.cpu[pcp.cpu_id];
        const u8 local_node = [&]() noexcept -> u8 {
            auto node = numa::node_for_cpu(pcp.cpu_id);
            return node ? *node : numa::NUMA_NODE_BOOT;
        }();
        arch::s390x::cpu::irq::irq_restore(setup_flags);
        sched::context::preempt_enable();

        sched::context::preempt_disable();
        const auto retire_flags = arch::s390x::cpu::irq::irq_save_disable();
        deactivate_cpu_slab(cache, cs);
        arch::s390x::cpu::irq::irq_restore(retire_flags);
        sched::context::preempt_enable();

        {
            sched::context::preempt_disable();
            const auto flags = arch::s390x::cpu::irq::irq_save_disable();

            if (cs.partial) {
                slab_header* sh  = cs.partial;
                cs.partial       = sh->cpu_partial_next;
                sh->cpu_partial_next = nullptr;
                cs.partial_count -= sh->objects - sh->inuse.load(memory_order::relaxed);
                auto& node = node_state(cache, sh->node_id);
                const sync::irq_lock_guard _{node.list_lock};
                activate_slab(cache, cs, sh);
                // Retry fast path inline.
                void* obj = cs.freelist;
                if (obj) {
                    cs.freelist = get_fp(cache, obj);
                    discard_value sh->inuse.fetch_add(1U, memory_order::relaxed);
                    arch::s390x::cpu::irq::irq_restore(flags);
                    sched::context::preempt_enable();
                    discard_value cache.stat_alloc_slow.fetch_add(1U, memory_order::relaxed);
                    discard_value cache.live_objects.fetch_add(1, memory_order::relaxed);
                    return obj;
                }
            }

            arch::s390x::cpu::irq::irq_restore(flags);
            sched::context::preempt_enable();
        }

        u16 inspected_nodes = 0U;
        for (u32 pass = 0U; pass < numa::MAX_NUMA_NODES; ++pass) {
            u8 node_id = numa::NUMA_NODE_ANY;
            u8 best_distance = 0xFFU;
            for (u8 candidate = 0U; candidate < numa::MAX_NUMA_NODES; ++candidate) {
                const u16 candidate_bit = static_cast<u16>(1U << candidate);
                if (inspected_nodes & candidate_bit) continue;
                const u8 distance = numa::node_distance(local_node, candidate);
                if (distance < best_distance) {
                    node_id = candidate;
                    best_distance = distance;
                }
            }
            if (node_id == numa::NUMA_NODE_ANY) break;
            inspected_nodes |= static_cast<u16>(1U << node_id);
            auto& node = node_state(cache, node_id);
            const sync::irq_lock_guard _{node.list_lock};

            if (node.partial) {
                slab_header* sh = node.partial;
                node_remove_partial(cache, sh);

                const u32 bulk_limit = cache.cpu_partial;
                u32       bulk_obj   = 0;

                sched::context::preempt_disable();
                const auto iflags = arch::s390x::cpu::irq::irq_save_disable();

                activate_slab(cache, cs, sh);

                while (node.partial && bulk_obj < bulk_limit) {
                    slab_header* extra = node.partial;
                    node_remove_partial(cache, extra);
                    extra->frozen = 1U;
                    extra->cpu_partial_next = cs.partial;
                    cs.partial = extra;
                    const u32 free_objects = extra->objects
                        - extra->inuse.load(memory_order::relaxed);
                    cs.partial_count += free_objects;
                    bulk_obj += free_objects;
                }

                void* obj = cs.freelist;
                if (obj) {
                    cs.freelist = get_fp(cache, obj);
                    discard_value sh->inuse.fetch_add(1U, memory_order::relaxed);
                    arch::s390x::cpu::irq::irq_restore(iflags);
                    sched::context::preempt_enable();
                    discard_value cache.stat_alloc_slow.fetch_add(1U, memory_order::relaxed);
                    discard_value cache.live_objects.fetch_add(1, memory_order::relaxed);
                    return obj;
                }
                arch::s390x::cpu::irq::irq_restore(iflags);
                sched::context::preempt_enable();
                __builtin_trap();
            }
        }

        slab_header* sh = alloc_new_slab(cache, local_node);
        if (!sh) [[unlikely]] return nullptr;

        sched::context::preempt_disable();
        const auto iflags = arch::s390x::cpu::irq::irq_save_disable();

        activate_slab(cache, cs, sh);
        void* obj = cs.freelist;
        if (obj) {
            cs.freelist = get_fp(cache, obj);
            discard_value sh->inuse.fetch_add(1U, memory_order::relaxed);
        }

        arch::s390x::cpu::irq::irq_restore(iflags);
        sched::context::preempt_enable();

        if (obj) {
            discard_value cache.stat_alloc_slow.fetch_add(1U, memory_order::relaxed);
            discard_value cache.live_objects.fetch_add(1, memory_order::relaxed);
        }
        return obj;
    }

    auto free_slow(kmem_cache& cache, slab_header* sh, void* obj) noexcept -> void {
        discard_value cache.stat_free_slow.fetch_add(1U, memory_order::relaxed);
        discard_value cache.live_objects.fetch_sub(1, memory_order::relaxed);

        auto& node = node_state(cache, sh->node_id);
        const sync::irq_lock_guard _{node.list_lock};

        set_fp(cache, obj, sh->freelist);
        sh->freelist = obj;
        const u32 prior = sh->inuse.fetch_sub(1U, memory_order::acq_rel);
        if (prior == 0U) __builtin_trap();
        const u32 inuse = prior - 1U;

        if (inuse == 0U && !sh->frozen && node.nr_partial > SLUB_MAX_PARTIAL) {
            node_remove_partial(cache, sh);
            free_slab(cache, sh);
            return;
        }

        if (!sh->frozen && !sh->on_node_partial) {
            node_add_partial(cache, sh);
        }
    }

} // namespace zxfoundation::memory::slub::detail

namespace zxfoundation::memory::slub {

    /// @brief Initialize the SLUB allocator.
    auto zi_slub_init(const sys::zinit::zi_desc&) noexcept
        -> sys::zinit::zi_result
    {
        using namespace detail;

        if (g_initialized) return {};

        constexpr u64 table_bytes = static_cast<u64>(sizeof(kmem_cache) * SLUB_MAX_CACHES);
        constexpr u32 total_pages = static_cast<u32>(
            (table_bytes + PAGE_SIZE - 1U) >> PAGE_SHIFT);
        u32 order = 0;
        while (order <= MAX_ORDER && (static_cast<u64>(1U) << order) < total_pages) {
            ++order;
        }
        if (order > MAX_ORDER) [[unlikely]] {
            return std::unexpected(
                lib::kernel_error::from_memory(lib::memory_error::out_of_memory));
        }

        auto folio_r = pmm::alloc(order, zone::normal, purpose::slab, page_init::zeroed);
        if (!folio_r) [[unlikely]] {
            return std::unexpected(
                lib::kernel_error::from_memory(lib::memory_error::out_of_memory));
        }

        g_cache_table_ptr = reinterpret_cast<kmem_cache*>(
            zxfoundation::memory::hhdm::phys_to_virt(folio_r->phys_base));

        g_cache_count = 0;
        g_initialized = true;
        return {};
    }
    __zinit_dep__(mm, p40, zi_slub_init, fatal, "zi_mmu_init")

    /// @brief Attach memory_object authorities to all live kmem_caches.
    auto zi_slub_attach_authorities(const sys::zinit::zi_desc&) noexcept -> sys::zinit::zi_result {
        using namespace detail;
        using namespace objects;
        if (!g_initialized) return {};
        const sync::irq_lock_guard _{g_cache_table_lock};
        for (u32 i = 0; i < g_cache_count; ++i) {
            kmem_cache& slot = g_cache_table_ptr[i];
            if (slot.authority) continue;
            auto mo_result = memory_object_create(
                scoms::kobject::kobject_type::kmem_cache, 0, nullptr, 0, 0);
            if (!mo_result) continue;
            slot.authority = memory_object_ref_of(**mo_result);
            auto view_result = memory_object_attach_view(
                slot.authority, memory_view_kind::kmem_cache, &slot);
            if (!view_result) {
                discard_value memory_object_destroy((*mo_result)->ko.id);
                slot.authority = {};
            }
        }
        return {};
    }
    __zinit_dep__(mm, p99, zi_slub_attach_authorities, fatal, "zi_slub_init")

    /// @brief Create a new slab cache.
    /// @param[in]  name         ASCII name for diagnostics.
    /// @param[in]  object_size  sizeof(T) for the object type.
    /// @param[in]  align        Required alignment.  0 → use object_size alignment.
    /// @param[in]  flags        slub_flags bitmask.
    [[nodiscard]] auto kmem_cache_create(
        const char*  name,
        u32          object_size,
        u32          align,
        slub_flags   flags
    ) noexcept -> std::expected<kmem_cache_ref_active, lib::kernel_error> {
        using namespace detail;
        using namespace pmm;

        if (!g_initialized || !name || object_size == 0 ||
            object_size > SLUB_MAX_SIZE)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::code::invalid_arg));

        u32 eff_align = (align > 0) ? align : 8U;
        if (!!(flags & slub_flags::hwcache_align))
            eff_align = std::max(eff_align,
                        static_cast<u32>(CACHE_LINE_SIZE));

        const u32 eff_size = lib::align_up(static_cast<u64>(object_size),
                                      static_cast<u64>(eff_align));
        const u32 fp_off   = 0U;

        u32 ord = 0;
        u32 obj_count = 0;
        while (ord <= SLUB_MAX_ORDER) {
            const u64 slab_bytes = static_cast<u64>(PAGE_SIZE) << ord;
            const u64 hdr_end    = lib::align_up(
                static_cast<u64>(sizeof(slab_header)),
                static_cast<u64>(eff_align));
            if (hdr_end < slab_bytes) {
                obj_count = static_cast<u32>((slab_bytes - hdr_end) / eff_size);
                if (obj_count >= 1) break;
            }
            ++ord;
        }
        if (obj_count == 0)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::code::invalid_arg));

        const sync::irq_lock_guard _{g_cache_table_lock};
        if (g_cache_count >= SLUB_MAX_CACHES)
            return std::unexpected(
                lib::kernel_error::from_memory(lib::memory_error::code::out_of_memory));

        const u64 rseq = arch::s390x::cpu::clock::stck()
                       ^ reinterpret_cast<u64>(name)
                       ^ static_cast<u64>(object_size)
                       ^ static_cast<u64>(g_cache_count);

        kmem_cache& slot = g_cache_table_ptr[g_cache_count];
        slot.name             = name;
        slot.object_size      = object_size;
        slot.size             = eff_size;
        slot.align            = eff_align;
        slot.fp_offset        = fp_off;
        slot.order            = ord;
        slot.objects_per_slab = obj_count;
        slot.flags            = flags;
        slot.random_seq       = rseq;
        slot.cpu_partial      = SLUB_CPU_PARTIAL_DEFAULT;


        kmem_cache_ref_embryo embryo_ref{&slot};
        auto ready_ref = embryo_ref.activate();
        ++g_cache_count;
        return ready_ref;
    }

    /// @brief Destroy a slab cache.
    auto kmem_cache_destroy(kmem_cache* cache) noexcept -> void {
        using namespace detail;
        if (!cache || !g_initialized) return;

        if (cache->live_objects.load(memory_order::acquire) != 0) {
            __builtin_trap();
        }

        constexpr u32 max_cpus = base::config::CONFIG_ZX_MAX_CPUS;

        for (u32 i = 0; i < max_cpus; ++i) {
            // Skip CPUs that have no registered lowcore (not online yet).
            if (!arch::s390x::cpu::lowcore::get_for_cpu(static_cast<u16>(i))) continue;
            kmem_cache_cpu& cs = cache->cpu[i];
            sched::context::preempt_disable();
            const auto iflags = arch::s390x::cpu::irq::irq_save_disable();

            deactivate_cpu_slab(*cache, cs);

            slab_header* sh = cs.partial;
            while (sh) {
                slab_header* nx = sh->cpu_partial_next;
                sh->frozen = 0U;
                sh->cpu_partial_next = nullptr;
                auto& node = node_state(*cache, sh->node_id);
                const sync::irq_lock_guard _{node.list_lock};
                if (sh->inuse.load(memory_order::acquire) < sh->objects)
                    node_add_partial(*cache, sh);
                sh = nx;
            }
            cs.partial = nullptr;
            cs.partial_count = 0;

            arch::s390x::cpu::irq::irq_restore(iflags);
            sched::context::preempt_enable();
        }

        for (u8 node_id = 0U; node_id < numa::MAX_NUMA_NODES; ++node_id) {
            auto& node = node_state(*cache, node_id);
            const sync::irq_lock_guard _{node.list_lock};
            slab_header* sh = node.partial;
            while (sh) {
                slab_header* nx = sh->next;
                node_remove_partial(*cache, sh);
                if (sh->inuse.load(memory_order::acquire) != 0U) __builtin_trap();
                free_slab(*cache, sh);
                sh = nx;
            }
        }

        if (cache->authority) {
            discard_value objects::memory_object_destroy(cache->authority);
            cache->authority = {};
        }
    }

    /// @brief Return empty node-partial slabs to PMM.
    auto kmem_cache_shrink(kmem_cache* cache) noexcept -> u32 {
        using namespace detail;
        if (!cache || !!(cache->flags & slub_flags::no_reap)) return 0U;

        u32 freed = 0;
        for (u8 node_id = 0U; node_id < numa::MAX_NUMA_NODES; ++node_id) {
            auto& node = node_state(*cache, node_id);
            const sync::irq_lock_guard _{node.list_lock};
            slab_header* sh = node.partial;
            while (sh) {
                slab_header* nx = sh->next;
                if (sh->inuse.load(memory_order::acquire) == 0U &&
                    node.nr_partial > node.min_partial) {
                    node_remove_partial(*cache, sh);
                    free_slab(*cache, sh);
                    ++freed;
                }
                sh = nx;
            }
        }
        return freed;
    }

    /// @brief Allocate one object from the given cache.
    /// @return  Pointer to the allocated object (HHDM virtual address),
    ///          or nullptr on OOM.
    [[nodiscard]] auto kmem_alloc(kmem_cache& cache) noexcept -> void* {
        using namespace detail;

        sched::context::preempt_disable();
        const auto iflags = arch::s390x::cpu::irq::irq_save_disable();
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto& cs  = cache.cpu[pcp.cpu_id];

        void* obj = cs.freelist;
        if (obj) [[likely]] {
            cs.freelist = get_fp(cache, obj);
            discard_value cs.slab->inuse.fetch_add(1U, memory_order::relaxed);
            ++cs.stat_fast_alloc;  // per-CPU non-atomic — aggregated on demand
            discard_value cache.live_objects.fetch_add(1, memory_order::relaxed);
            arch::s390x::cpu::irq::irq_restore(iflags);
            sched::context::preempt_enable();
            return obj;
        }

        arch::s390x::cpu::irq::irq_restore(iflags);
        sched::context::preempt_enable();

        return alloc_slow(cache);
    }

    /// @brief Typed allocation helper — returns a pointer to T.
    /// @tparam T  Object type.  sizeof(T) must equal cache.object_size.
    /// @brief Free one object back to its cache.
    /// @param[in]  cache  The cache the object was allocated from.
    /// @param[in]  obj    Pointer to the object to free.
    auto kmem_free(kmem_cache& cache, void* obj) noexcept -> void {
        using namespace detail;
        if (!obj) [[unlikely]] return;

        slab_header* sh = slab_of(cache, obj);

        sched::context::preempt_disable();
        const auto iflags = arch::s390x::cpu::irq::irq_save_disable();
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto& cs  = cache.cpu[pcp.cpu_id];

        if (sh == cs.slab) [[likely]] {
            set_fp(cache, obj, cs.freelist);
            cs.freelist = obj;
            const u32 prior = sh->inuse.fetch_sub(1U, memory_order::relaxed);
            if (prior == 0U) __builtin_trap();
            ++cs.stat_fast_free;  // per-CPU non-atomic
            discard_value cache.live_objects.fetch_sub(1, memory_order::relaxed);
            arch::s390x::cpu::irq::irq_restore(iflags);
            sched::context::preempt_enable();
            return;
        }

        arch::s390x::cpu::irq::irq_restore(iflags);
        sched::context::preempt_enable();

        free_slow(cache, sh, obj);
    }

    /// @brief Return the number of live (allocated) objects in a cache.
    [[nodiscard]] auto kmem_cache_live_objects(
        const kmem_cache& cache
    ) noexcept -> i64 {
        return cache.live_objects.load(memory_order::relaxed);
    }

    /// @brief Return a pointer to the Nth registered cache, or nullptr.
    [[nodiscard]] auto kmem_get_cache(u32 idx) noexcept -> kmem_cache* {
        using namespace detail;
        if (!g_initialized || idx >= g_cache_count) return nullptr;
        return &g_cache_table_ptr[idx];
    }

    /// @brief Return the total number of registered caches.
    [[nodiscard]] auto kmem_cache_count() noexcept -> u32 {
        return detail::g_cache_count;
    }

    /// @brief Free a slab-allocated object without knowing its cache.
    /// @param[in] ptr  Object pointer previously returned by kmem_alloc.
    auto kmem_free_raw(void* ptr) noexcept -> void {
        using namespace detail;
        if (!ptr) [[unlikely]] return;

        const u64 phys = hhdm::virt_to_phys(reinterpret_cast<u64>(ptr));

        for (u32 order = 0U; order <= SLUB_MAX_ORDER; ++order) {
            const u64 mask = ~((static_cast<u64>(PAGE_SIZE) << order) - 1ULL);
            const u64 slab_phys = phys & mask;
            auto* sh = reinterpret_cast<slab_header*>(hhdm::phys_to_virt(slab_phys));
            if (!sh || sh->phys != slab_phys || sh->slab_order != order ||
                !sh->cache || sh->cache->order != order) {
                continue;
            }
            const auto begin = reinterpret_cast<u64>(first_object(*sh->cache, sh));
            const auto end = reinterpret_cast<u64>(sh) + (PAGE_SIZE << order);
            const auto address = reinterpret_cast<u64>(ptr);
            if (address < begin || address >= end ||
                ((address - begin) % sh->cache->size) != 0U) {
                zxfoundation::sys::syschk::syschk_fatal(
                    lib::kernel_error::from_memory(lib::memory_error::corruption),
                    "slub: kmem_free_raw ptr={:#x} violates slab layout (begin={:#x} end={:#x} size={})",
                    address, begin, end, sh->cache->size);
            }
            kmem_free(*sh->cache, ptr);
            return;
        }

        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_memory(lib::memory_error::corruption),
            "slub: kmem_free_raw ptr={:#x} — no slab header claims the object",
            reinterpret_cast<u64>(ptr));
    }
} // namespace zxfoundation::memory::slub

