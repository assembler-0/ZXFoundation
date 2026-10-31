/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sync/qspinlock.zcomponent/core.cxx
/// @brief Queued spinlock (MCS-based) for z/Architecture.

module zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.qspinlock.mcs;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sched.context.core;
import zxfoundation.sys.syschk.barebones;
import zxfoundation.memory.hhdm;
import arch.s390x.cpu.sync;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.irq;
import lib.bit;
import lib.stl;

#define __zxmacro_want_assert
#include macro_extensions

namespace zxfoundation::sync::qspinlock::detail {
    static constexpr u32 _Q_LOCKED_VAL   = 1U;      ///< Bit 0: lock held.
    static constexpr u32 _Q_PENDING_VAL  = 2U;      ///< Bit 1: pending waiter.
    static constexpr u32 _Q_LOCKED_MASK  = _Q_LOCKED_VAL | _Q_PENDING_VAL;

    static constexpr u32 _Q_TAIL_OFFSET  = 2U;
    static constexpr u32 _Q_TAIL_MASK    = ~static_cast<u32>(_Q_LOCKED_MASK);

    static constexpr u32 _Q_NODE_BITS    = 4U;
    static constexpr u32 _Q_NODE_MASK    = (1U << _Q_NODE_BITS) - 1U;  // 0xF for the 16-node encoding
    static constexpr u32 _Q_TAIL_CPU_BITS = 32U - _Q_TAIL_OFFSET - _Q_NODE_BITS;

    static_assert(
        zxfoundation::base::config::CONFIG_ZX_MAX_CPUS < (1U << _Q_TAIL_CPU_BITS),
        "qspinlock tail encoding cannot represent every configured CPU");
    static_assert(
        MCS_MAX_LOCK_DEPTH <= (1U << _Q_NODE_BITS),
        "qspinlock tail encoding cannot represent every MCS node");

    /// @brief Encode (cpu_id + 1, node_idx) into a tail value shifted into
    ///        the correct position in the lock word.
    /// @param[in] cpu_id   Logical CPU ID (0-based).
    /// @param[in] node_idx MCS node allocation index (0–3).
    /// @return Encoded tail value suitable for OR-ing into the lock word.
    static constexpr auto encode_tail(const u16 cpu_id, const u32 node_idx) noexcept -> u32 {
        const u32 tail_raw = ((static_cast<u32>(cpu_id) + 1U) << _Q_NODE_BITS)
                             | (node_idx & _Q_NODE_MASK);
        return tail_raw << _Q_TAIL_OFFSET;
    }

    /// @brief Decode the CPU index from a full lock word.
    /// @param[in] lock_val The full 32-bit lock word.
    /// @return The logical CPU ID that owns the tail position.
    static constexpr auto decode_tail_cpu(const u32 lock_val) noexcept -> u16 {
        const u32 tail_raw = (lock_val & _Q_TAIL_MASK) >> _Q_TAIL_OFFSET;
        return static_cast<u16>((tail_raw >> _Q_NODE_BITS) - 1U);
    }

    /// @brief Decode the node index from a full lock word.
    /// @param[in] lock_val The full 32-bit lock word.
    /// @return The MCS node index (0–3) for the tail owner.
    static constexpr auto decode_tail_node(const u32 lock_val) noexcept -> u32 {
        const u32 tail_raw = (lock_val & _Q_TAIL_MASK) >> _Q_TAIL_OFFSET;
        return tail_raw & _Q_NODE_MASK;
    }
}

/// @name private class qspinlock
/// @{
namespace zxfoundation::sync::qspinlock {

    /// @brief Atomically transition the lock word from PENDING|LOCKED to LOCKED.
    /// @pre  PENDING bit is set, LOCKED bit is clear, TAIL field is zero.
    /// @post LOCKED bit is set, PENDING bit is clear.
    template <zxfoundation::sync::lock_rank Rank>
    auto qspinlock<Rank>::pending_to_locked() noexcept -> void {
        u32 expected = m_val.load(std::memory_order::relaxed);
        for (;;) {
            if (expected & detail::_Q_LOCKED_VAL) {
                arch::s390x::cpu::sync::cpu_relax();
                expected = m_val.load(std::memory_order::relaxed);
                continue;
            }
            const u32 desired = (expected & ~detail::_Q_PENDING_VAL) | detail::_Q_LOCKED_VAL;
            if (m_val.compare_exchange_weak(
                    expected, desired,
                    std::memory_order::acquire, std::memory_order::relaxed)) {
                return;
                    }
            arch::s390x::cpu::sync::cpu_relax();
        }
    }

    /// @brief Slow acquisition path: pending-bit optimization + MCS queue.
    /// @pre  Interrupts MUST be disabled (asserted by caller).
    /// @pre  Fast-path CAS has already failed.
    /// @post The lock is held with _Q_LOCKED_VAL set.
    template <zxfoundation::sync::lock_rank Rank>
    auto qspinlock<Rank>::lock_slow() noexcept -> void {
        u32 v = m_val.load(std::memory_order::relaxed);

        if ((v & detail::_Q_LOCKED_MASK) == detail::_Q_LOCKED_VAL && !(v & detail::_Q_TAIL_MASK)) {
            u32 expected_pending = detail::_Q_LOCKED_VAL;
            if (m_val.compare_exchange_strong(
                    expected_pending,
                    detail::_Q_LOCKED_VAL | detail::_Q_PENDING_VAL,
                    std::memory_order::acquire,
                    std::memory_order::relaxed)) {
                // Spin until the current holder clears LOCKED.
                while (m_val.load(std::memory_order::acquire) & detail::_Q_LOCKED_VAL)
                    arch::s390x::cpu::sync::cpu_relax();

                pending_to_locked();
                return;
                    }
            v = m_val.load(std::memory_order::relaxed);
        }

        auto& pcp = arch::s390x::cpu::percpu::this_cpu();

        const u32 node_idx = pcp.lock_depth;
        if (node_idx >= MCS_MAX_LOCK_DEPTH) {
            for (;;) {
                u32 expected = 0U;
                if (m_val.compare_exchange_weak(
                        expected, detail::_Q_LOCKED_VAL,
                        std::memory_order::acquire,
                        std::memory_order::relaxed))
                    return;
                arch::s390x::cpu::sync::cpu_relax();
            }
        }
        pcp.lock_depth++;

        mcs_node* const node = &pcp.qnodes[node_idx];

        // Initialize the node: locked=1 means "I am waiting", next=nullptr.
        node->locked.store(1, std::memory_order::relaxed);
        node->next.store(nullptr, std::memory_order::relaxed);

        const u64 node_offset = reinterpret_cast<u64>(node) -
                                reinterpret_cast<u64>(&pcp);
        const u64 node_phys = pcp.self_phys + node_offset;
        auto* const node_absolute = reinterpret_cast<mcs_node*>(
            zxfoundation::memory::hhdm::phys_to_virt(node_phys));

        // Encode our position into the lock word's tail field.
        const u32 my_tail = detail::encode_tail(pcp.cpu_id, node_idx);

        // Atomically install our tail, preserving the LOCKED/PENDING bits.
        u32 old_val = m_val.load(std::memory_order::relaxed);
        u32 new_val;
        do {
            new_val = (old_val & detail::_Q_LOCKED_MASK) | my_tail;
        } while (!m_val.compare_exchange_weak(
                     old_val, new_val, std::memory_order::acq_rel, std::memory_order::relaxed));

        if (old_val & detail::_Q_TAIL_MASK) {
            const u16 prev_cpu = detail::decode_tail_cpu(old_val);
            const u32 prev_idx = detail::decode_tail_node(old_val);

            arch::s390x::cpu::percpu::percpu_data* prev_pcp =
                arch::s390x::cpu::percpu::cpu_percpu(prev_cpu);
            chk_on(!prev_pcp);

            mcs_node* const prev_node = &prev_pcp->qnodes[prev_idx];

            prev_node->next.store(node_absolute, std::memory_order::release);

            while (node->locked.load(std::memory_order::acquire))
                arch::s390x::cpu::sync::cpu_relax();
        }

        while (m_val.load(std::memory_order::acquire) & detail::_Q_LOCKED_MASK)
            arch::s390x::cpu::sync::cpu_relax();

        u32 expected_solo = my_tail;
        if (m_val.compare_exchange_strong(
                expected_solo, detail::_Q_LOCKED_VAL,
                std::memory_order::acquire, std::memory_order::relaxed)) {
            pcp.lock_depth--;
            return;   // sole waiter, no successor to wake
                }

        discard_value m_val.fetch_or(detail::_Q_LOCKED_VAL, std::memory_order::acquire);

        mcs_node* next_node;
        while (!((next_node = node->next.load(std::memory_order::acquire))))
            arch::s390x::cpu::sync::cpu_relax();

        // Pass the lock to the successor by clearing its locked flag.
        next_node->locked.store(0, std::memory_order::release);

        pcp.lock_depth--;
    }
}
/// @}

/// @name public class qspinlock
/// @{
namespace zxfoundation::sync::qspinlock {

    /// @brief Acquire the lock (blocking, SMP-safe).
    /// @pre Interrupts MUST be disabled (enforced by assert).
    /// @post The lock is held.  Caller MUST call unlock() exactly once.
    template <zxfoundation::sync::lock_rank Rank>
    auto qspinlock<Rank>::lock() noexcept -> void {
        sched::context::assert_irqs_disabled();

        u32 expected = 0;
        if (m_val.compare_exchange_strong(
                expected, detail::_Q_LOCKED_VAL, std::memory_order::acquire, std::memory_order::relaxed))
            return;
        lock_slow();
    }

    /// @brief Try to acquire the lock without blocking.
    /// @return true if the lock was acquired, false if it was already held.
    template <zxfoundation::sync::lock_rank Rank>
    [[nodiscard]] auto qspinlock<Rank>::try_lock() noexcept -> bool {
        sched::context::assert_irqs_disabled();
        u32 expected = 0;
        return m_val.compare_exchange_strong(
            expected, detail::_Q_LOCKED_VAL, std::memory_order::acquire, std::memory_order::relaxed);
    }

    /// @brief Release the lock.
    /// @pre The caller holds the lock.
    /// @post The lock is released.
    template <zxfoundation::sync::lock_rank Rank>
    auto qspinlock<Rank>::unlock() noexcept -> void {
        discard_value m_val.fetch_and(~detail::_Q_LOCKED_VAL, std::memory_order::release);
    }

    /// @brief Test whether the lock is held (non-deterministic snapshot).
    /// @return true if the LOCKED bit is set at the time of the load.
    /// @note This is a snapshot; the lock state may change immediately after.
    template <zxfoundation::sync::lock_rank Rank>
    [[nodiscard]] auto qspinlock<Rank>::is_locked() const noexcept -> bool {
        return (m_val.load(std::memory_order::relaxed) & detail::_Q_LOCKED_VAL) != 0;
    }

    /// @brief Acquire the lock and save + disable IRQs atomically.
    /// @return Saved irqflags (pass to unlock_irqrestore()).
    template <zxfoundation::sync::lock_rank Rank>
    auto qspinlock<Rank>::lock_irqsave() noexcept -> irqflags {
        const irqflags flags = arch::s390x::cpu::irq::irq_save_disable();
        lock();
        return flags;
    }

    /// @brief Release the lock and restore IRQs.
    /// @param[in] flags  Value returned by lock_irqsave().
    template <zxfoundation::sync::lock_rank Rank>
    auto qspinlock<Rank>::unlock_irqrestore(const irqflags flags) noexcept -> void {
        unlock();
        arch::s390x::cpu::irq::irq_restore(flags);
    }


    // Explicit instantiation per lock class rank
    template class qspinlock<zxfoundation::sync::lock_rank::svc_table>;
    template class qspinlock<zxfoundation::sync::lock_rank::cap_table>;
    template class qspinlock<zxfoundation::sync::lock_rank::policy_table>;
    template class qspinlock<zxfoundation::sync::lock_rank::ipc_table>;
    template class qspinlock<zxfoundation::sync::lock_rank::page_cache>;
    template class qspinlock<zxfoundation::sync::lock_rank::scoms_table>;
    template class qspinlock<zxfoundation::sync::lock_rank::scoms_meta>;
    template class qspinlock<zxfoundation::sync::lock_rank::idalloc>;
    template class qspinlock<zxfoundation::sync::lock_rank::css_table>;
    template class qspinlock<zxfoundation::sync::lock_rank::aste_table>;
    template class qspinlock<zxfoundation::sync::lock_rank::irq_desc>;
    template class qspinlock<zxfoundation::sync::lock_rank::smp_doorbell>;
    template class qspinlock<zxfoundation::sync::lock_rank::rq>;
    template class qspinlock<zxfoundation::sync::lock_rank::rq_context>;
    template class qspinlock<zxfoundation::sync::lock_rank::cpudl>;
    template class qspinlock<zxfoundation::sync::lock_rank::vm_space>;
    template class qspinlock<zxfoundation::sync::lock_rank::rmap_stripe>;
    template class qspinlock<zxfoundation::sync::lock_rank::pmm_zone>;
    template class qspinlock<zxfoundation::sync::lock_rank::slub_cache>;
    template class qspinlock<zxfoundation::sync::lock_rank::slub_node>;
    template class qspinlock<zxfoundation::sync::lock_rank::scoms_kobject>;
    template class qspinlock<zxfoundation::sync::lock_rank::portal_ar>;
    template class qspinlock<zxfoundation::sync::lock_rank::skey>;
    template class qspinlock<zxfoundation::sync::lock_rank::rtmutex_pi>;
    template class qspinlock<zxfoundation::sync::lock_rank::turnstile>;
    template class qspinlock<zxfoundation::sync::lock_rank::seqlock_writer>;
    template class qspinlock<zxfoundation::sync::lock_rank::printk>;
    template class qspinlock<zxfoundation::sync::lock_rank::printk_flush>;
    template class qspinlock<zxfoundation::sync::lock_rank::cxxrt>;
    template class qspinlock<zxfoundation::sync::lock_rank::diag>;
    template class qspinlock<zxfoundation::sync::lock_rank::tx_fallback>;
    template class qspinlock<zxfoundation::sync::lock_rank::mutex_internal>;
    template class qspinlock<zxfoundation::sync::lock_rank::gate_defer>;

} // namespace zxfoundation::sync::qspinlock
/// @}
