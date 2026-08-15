/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/locking/mutex.cxxm
/// @brief Blocking mutex — sleep/wake lock with wait-queue integration.

module zxfoundation.sync.mutex.core;
import zxfoundation.base.types;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.waitqueue.types;
import zxfoundation.domain.strand_types;
import zxfoundation.domain.domain_types;
import zxfoundation.sync.lockable;
import zxfoundation.sched.context;
import zxfoundation.sched.sched;
import zxfoundation.base.typestate;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.irq;
import lib.list;
import lib.container_of;
import std;

namespace zxfoundation::sync::mutex {

    using zxfoundation::domain::strand;
    using zxfoundation::sync::waitqueue::wait_entry;
    using zxfoundation::domain::strand;
    using zxfoundation::domain::sched_substate;
    using zxfoundation::base::lifecycle_state;
    using zxfoundation::domain::domain;
    using zxfoundation::domain::domain_flags;
    using zxfoundation::sync::waitqueue::wait_head;
    using zxfoundation::sync::waitqueue::wait_entry;
    using zxfoundation::sync::waitqueue::wait_flags;
    using zxfoundation::sync::waitqueue::wait_result;
    using std::atomic;
    using std::memory_order;

    struct mutex_waiter {
        wait_entry entry{};
        strand*    strand_ptr{nullptr};
    };

    /// @brief Acquire the mutex, blocking if contended.
    /// @pre  can_sleep() must hold (IRQs enabled, not in interrupt,
    ///       preemption enabled, not in RCU read-side).
    /// @post The mutex is held.  Caller call unlock() exactly once.
    auto mutex::lock() noexcept -> void {
        sched::context::assert_can_sleep();

        const irqflags flags = arch::s390x::cpu::irq::irq_save_disable();

        u32 expected = 0;
        if (m_val.compare_exchange_strong(
                expected, 1, memory_order::acquire, memory_order::relaxed)) {
            arch::s390x::cpu::irq::irq_restore(flags);
            return;
        }

        slow_lock(flags);
    }

    /// @brief Try to acquire the mutex without blocking.
    /// @return true if the mutex was acquired.
    [[nodiscard]] auto mutex::try_lock() noexcept -> bool {
        u32 expected = 0;
        return m_val.compare_exchange_strong(
            expected, 1, memory_order::acquire, memory_order::relaxed);
    }

    /// @brief Release the mutex and wake the first waiter, if any.
    /// @pre The caller holds the mutex.
    /// @post The mutex is released.
    auto mutex::unlock() noexcept -> void {
        strand* to_wake = nullptr;

        {
            wait_entry* entry = nullptr;

            {
                sync::irq_lock_guard wq_guard(m_wq.lock);

                if (!m_wq.entries.empty()) {
                    lib::list_node* node = *m_wq.entries.begin();
                    entry = lib::container_of<wait_entry,
                        lib::list_node, &wait_entry::link>(node);

                    entry->result = wait_result::woken;
                    entry->active = false;
                    lib::list_head::remove(*node);
                }

                m_val.store(0, memory_order::release);
            }

            if (entry) {
                auto* waiter = lib::container_of<mutex_waiter,
                    wait_entry, &mutex_waiter::entry>(entry);
                to_wake = waiter->strand_ptr;
            }
        }

        if (to_wake) {
            to_wake->substate = sched_substate::runnable;
            zxfoundation::sched::enqueue_strand(to_wake);

            auto* owner_dom = static_cast<domain*>(to_wake->owner);
            if (owner_dom) {
                owner_dom->flags |= domain_flags::need_resched;
            }
        }
    }

    /// @brief Non-deterministic snapshot: true if the mutex appears held.
    [[nodiscard]] auto mutex::is_locked() const noexcept -> bool {
        return m_val.load(memory_order::relaxed) != 0;
    }

    /// @brief Slow path: add to wait queue, block, re-contend.
    auto mutex::slow_lock(const irqflags flags) noexcept -> void {
        auto& pcp = arch::s390x::cpu::lowcore::this_cpu_percpu();
        auto* current = reinterpret_cast<strand*>(pcp.current_strand);

        mutex_waiter w{};
        w.strand_ptr = current;

        const auto* lc = arch::s390x::cpu::lowcore::get_current();
        if (lc->current_domain != 0) {
            w.entry.domain_id =
                *reinterpret_cast<const u32*>(lc->current_domain);
        }

        {
            sync::irq_lock_guard wq_guard(m_wq.lock);

            u32 expected = 0;
            if (m_val.compare_exchange_strong(
                    expected, 1, memory_order::acquire, memory_order::relaxed)) {
                arch::s390x::cpu::irq::irq_restore(flags);
                return;
            }

            w.entry.flags = wait_flags::exclusive;
            w.entry.active = true;
            m_wq.entries.push_back(w.entry.link);
        }

        for (;;) {
            {
                sync::irq_lock_guard wq_guard(m_wq.lock);

                u32 expected = 0;
                if (m_val.compare_exchange_strong(
                        expected, 1, memory_order::acquire, memory_order::relaxed)) {
                    if (w.entry.active) {
                        lib::list_head::remove(w.entry.link);
                        w.entry.active = false;
                    }
                    arch::s390x::cpu::irq::irq_restore(flags);
                    return;
                }

                if (!w.entry.active) {
                    w.entry.active = true;
                    m_wq.entries.push_back(w.entry.link);
                }
            }

            current->substate = sched_substate::idle;
            zxfoundation::sched::schedule();
        }
    };

} // namespace zxfoundation::sync::mutex