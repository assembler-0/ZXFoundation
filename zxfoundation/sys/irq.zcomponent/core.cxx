/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/irq.zcomponent/irqdesc.cxxm
/// @brief Generic IRQ descriptor table

module zxfoundation.sys.irq.core;
import zxfoundation.sys.irq.consts;
import zxfoundation.sys.irq.desc;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.table;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.scoms.metadata.core;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.irq;
import lib.error;
import std;

no_mangle namespace zxfoundation::sys::irq::detail {

    from_global(reference) scoms::scoms_table<irq_desc, IRQ_NR_MAX> g_irq_table;
    from_global(reference) bool g_initialized;

} // namespace zxfoundation::sys::irq::detail

namespace zxfoundation::sys::irq {

    /// @brief Dispatch an IRQ — called from C interrupt entry stubs (trap.cxxm).
    /// @param[in] irq   Vector number inside [0,IRQ_NR_MAX]
    /// @param[in] frame Pointer to the saved interrupt frame (entry.S layout).
    /// @param[in] cls   Architectural interruption class (PoP §6 "Interruption
    ///                  Definitions").  Determines IRQ-nesting accounting width.
    auto irq_dispatch(
        u16                               irq,
        arch::s390x::cpu::irq::irq_frame* frame,
        arch::s390x::cpu::irq::irq_class  cls
    ) noexcept -> void {
        using namespace detail;
        if (irq >= IRQ_NR_MAX) [[unlikely]] return;

        auto& pcp = arch::s390x::cpu::lowcore::this_cpu_percpu();
        const u32 raise =
            (cls == arch::s390x::cpu::irq::ZX_IRQ_CLASS_MCCK) ? 2U : 1U;
        pcp.irq_nesting += raise;

        handler_fn h = nullptr;
        void*      d = nullptr;
        u32        flags_to_check = 0U;

        irq_desc* desc = g_irq_table.find_raw(irq);
        if (desc) [[likely]] {
            if (desc->flags & IRQF_FAST) {
                h = desc->handler;
                d = desc->data;
                flags_to_check = desc->flags;
                discard_value desc->count.fetch_add(1,
                    std::memory_order::relaxed);
            } else {
                const sync::irq_lock_guard guard{desc->lock};
                h = desc->handler;
                d = desc->data;
                flags_to_check = desc->flags;
                discard_value desc->count.fetch_add(1,
                    std::memory_order::relaxed);
            }
        }

        if (h) [[likely]] h(irq, frame, d);
        discard_value flags_to_check;

        pcp.irq_nesting -= raise;
    }

    /// @brief Register a handler for a vector.
    /// @param[in] irq     Vector slot within the relevant IRQ_BASE_* span.
    /// @param[in] handler Cancellable handler.  Must not block indefinitely;
    ///                    the dispatch path is IRQ-disabled.
    /// @param[in] data    Opaque handler-private cookie.
    /// @param[in] flags   Bitmask of @c IRQF_* values.
    [[nodiscard]] auto irq_register(
        u16        irq,
        handler_fn handler,
        void*      data,
        u32        flags
    ) noexcept -> std::expected<void, lib::kernel_error> {
        using namespace detail;

        if (!g_initialized)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::not_found));
        if (irq >= IRQ_NR_MAX || !handler)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        irq_desc* d = g_irq_table.find_raw(irq);
        if (!d) {
            auto result = g_irq_table.alloc_at(irq);
            if (!result) [[unlikely]]
                return std::unexpected(
                    lib::kernel_error::from_memory(
                        lib::memory_error::out_of_memory));
            auto embryo_ref = static_cast<
                scoms::kobject::robject<irq_desc, base::lifecycle_state::embryo>&&
            >(result.value());
            d = &(*embryo_ref);
            d->irq_nr = irq;
            auto owner_result = scoms::object_core::set_owner(d->ko, 0U);
            if (!owner_result) {
                discard_value g_irq_table.release(irq);
                return std::unexpected(owner_result.error());
            }
            discard_value static_cast<
                scoms::kobject::robject<irq_desc, base::lifecycle_state::embryo>&&
            >(embryo_ref).activate();
        }

        const sync::irq_lock_guard desc_guard{d->lock};

        if (d->registered() && !(flags & IRQF_SHARED))
            return std::unexpected(
                lib::kernel_error::from_locking(
                    lib::locking_error::already_locked));

        d->handler = handler;
        d->data    = data;
        d->flags   = flags;
        return {};
    }

    /// @brief Unregister the handler for a vector and release the SCOMS slot.
    /// @param[in] irq  Vector slot to clear.
    auto irq_unregister(u16 irq) noexcept -> void {
        using namespace detail;
        if (!g_initialized || irq >= IRQ_NR_MAX) return;

        irq_desc* d = g_irq_table.find_raw(irq);
        if (!d) return;

        {
            const sync::irq_lock_guard desc_guard{d->lock};
            d->handler = nullptr;
            d->data    = nullptr;
            d->flags   = 0;
        }

        discard_value g_irq_table.release(irq);
    }

    /// @brief Initialize the IRQ subsystem with the SCOMS scoms_table.
    /// @note Idempotent — safe to invoke from `main` after `printk::init()`.
    auto init() noexcept -> void {
        using namespace detail;
        if (g_initialized) return;
        g_irq_table.init(scoms::kobject::kobject_type::irq_desc);
        g_initialized = true;
    }

    /// @brief Set one deferred-work bit on the calling CPU's
    ///        @c percpu::s390x_percpu_data::deferred_pending field.
    /// @param[in] bit  One of @c DEFERRED_*_BIT masks above.
    /// @note Memory order is release so the bit set is visible before any
    ///       IRQ return path context restore.
    auto deferred_set(u32 bit) noexcept -> void {
        auto& pcp = arch::s390x::cpu::lowcore::this_cpu_percpu();
        discard_value pcp.deferred_pending.fetch_or(bit,
            std::memory_order::release);
    }

    /// @brief Test-and-clear a deferred-work bit.
    /// @return @c true if the bit was set (and is now cleared).
    /// @note Drains the bit atomically so the caller cannot accidentally
    ///       re-process work after a context switch.
    [[nodiscard]] auto deferred_test_clear(u32 bit) noexcept -> bool {
        auto& pcp = arch::s390x::cpu::lowcore::this_cpu_percpu();
        const u32 old = pcp.deferred_pending.fetch_and(~bit,
            std::memory_order::acq_rel);
        return (old & bit) != 0U;
    }

} // namespace zxfoundation::sys::irq