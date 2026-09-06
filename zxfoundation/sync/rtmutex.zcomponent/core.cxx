/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/locking/rtmutex.cxxm
/// @brief Real-time mutex with multi-link priority inheritance.

module zxfoundation.sync.rtmutex.core;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.domain.core;
import zxfoundation.sched.core;
import zxfoundation.sched.rt.core;
import zxfoundation.sched.policy;
import zxfoundation.sched.context.core;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.isolation;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.irq;
import lib.list;
import lib.error;
import lib.container_of;
import lib.stl;

namespace zxfoundation::sync::detail {

    using zxfoundation::dgp::domain::strand;
    using zxfoundation::dgp::domain::domain;
    using zxfoundation::sched::sched_class_id;

    /// @brief Recover the enclosing @c strand from its @c pi_waiters node.
    [[nodiscard]] no_export inline auto strand_from_pi_waiters(lib::list_node* n) noexcept -> strand* {
        auto* body = lib::container_of<
            zxfoundation::dgp::domain::strand_body,
            lib::list_node,
            &zxfoundation::dgp::domain::strand_body::pi_waiters>(n);
        return reinterpret_cast<strand*>(
            reinterpret_cast<u8*>(body) -
            __builtin_offsetof(zxfoundation::dgp::domain::strand, body));
    }

    /// @brief Defensive upper bound on the PI chain length.  Real lock
    ///        nesting depths are tiny; this is a pure DoS guard.
    no_export constexpr u32 MAX_PI_DEPTH = 16U;

    /// @brief Effective RT priority of a strand — boosted_prio if set,
    ///        else @c sched_policy.rt_priority when class is rt, else 0.
    [[nodiscard]] no_export auto effective_rt_prio(const strand& s) noexcept -> u8 {
        if (s.body && s.body->boosted_prio != 0U) {
            return s.body->boosted_prio;
        }
        if (s.sched_policy.class_id == sched_class_id::rt) {
            return s.sched_policy.rt_priority;
        }
        return 0U;
    }

    /// @brief DGP cross-domain PI propagation gate.
    [[nodiscard]] no_export auto pi_may_cross(const strand& waiter, const strand& owner) noexcept -> bool {
        const auto* wd = static_cast<const domain*>(waiter.owner);
        const auto* od = static_cast<const domain*>(owner.owner);
        if (!wd || !od) return true;
        if (wd == od) return true;
        using zxfoundation::dgp::isolation::isolation_rights;
        using zxfoundation::dgp::isolation::has_all_rights;
        return has_all_rights(od->granted_rights, dgp::isolation::isolation_rights::delegate)
            && has_all_rights(wd->granted_rights, dgp::isolation::isolation_rights::enter);
    }

    /// @brief Highest effective RT priority among the waiters of @c m.
    ///        Caller must hold @c m.pi_lock.
    [[nodiscard]] no_export auto top_waiter_prio(const lib::list_head& waiters) noexcept -> u8 {
        u8 best = 0U;
        for (auto waiter : waiters) {
            auto* ws = strand_from_pi_waiters(waiter);
            const u8 p = effective_rt_prio(*ws);
            if (p > best) best = p;
        }
        return best;
    }

    /// @brief Re-apply the boost on @c owner given the current top-waiter
    ///        priority @c new_prio of the mutex it holds.
    no_export auto reboost_owner(strand& owner, u8 new_prio, const strand& top_waiter) noexcept -> void {
        if (new_prio == 0U) {
            zxfoundation::sched::rt::unboost_prio(owner);
            return;
        }
        if (!pi_may_cross(top_waiter, owner)) {
            return;
        }
        zxfoundation::sched::rt::boost_prio(owner, new_prio);
    }

} // namespace zxfoundation::sync::detail


/// @name public class rtmutex
/// @{
namespace zxfoundation::sync::rtmutex {

    using std::atomic;
    using std::memory_order;
    using zxfoundation::dgp::domain::strand;
    using zxfoundation::dgp::domain::domain;
    using zxfoundation::sched::sched_substate;
    using zxfoundation::dgp::domain::domain_flags;
    using zxfoundation::sched::sched_class_id;

    /// @brief Acquire the mutex, blocking if held.
    /// @pre  can_sleep() (IRQs enabled, not in IRQ, preemption on).
    auto rtmutex::lock() noexcept -> void {
        zxfoundation::sched::context::assert_can_sleep();
        const irqflags flags = arch::s390x::cpu::irq::irq_save_disable();

        u32 expected = 0U;
        if (m_lock_word.compare_exchange_strong(
                expected, 1U, memory_order::acquire, memory_order::relaxed)) {
            auto& pcp = arch::s390x::cpu::percpu::this_cpu();
            m_owner = reinterpret_cast<strand*>(pcp.current_strand);
            arch::s390x::cpu::irq::irq_restore(flags);
            return;
                }

        slow_lock(flags);
    }

    /// @brief Non-blocking attempt.
    [[nodiscard]] auto rtmutex::try_lock() noexcept -> bool {
        u32 expected = 0U;
        if (m_lock_word.compare_exchange_strong(
                expected, 1U, memory_order::acquire, memory_order::relaxed)) {
            auto& pcp = arch::s390x::cpu::percpu::this_cpu();
            m_owner = reinterpret_cast<strand*>(pcp.current_strand);
            return true;
                }
        return false;
    }

    /// @brief Release the mutex.
    auto rtmutex::unlock() noexcept -> void {
        using detail::top_waiter_prio;
        using detail::reboost_owner;
        using detail::pi_may_cross;
        using detail::effective_rt_prio;
        using detail::MAX_PI_DEPTH;

        strand* new_owner = nullptr;

        {
            strand* old_owner = nullptr;
            sync::irq_lock_guard pi_guard(m_pi_lock);

            if (!m_waiters.empty()) {
                lib::list_node* node = *m_waiters.begin();
                lib::list_head::remove(*node);
                new_owner = detail::strand_from_pi_waiters(node);
                new_owner->body->blocked_on = nullptr;
            }

            old_owner = m_owner;
            m_owner   = new_owner;

            if (old_owner) {
                zxfoundation::sched::rt::unboost_prio(*old_owner);

                strand* cur = old_owner;
                rtmutex* held_lock = this;  // holds lock on `held_lock` already.
                for (u32 depth = 0U; depth < MAX_PI_DEPTH; ++depth) {
                    void* blk = cur->body ? cur->body->blocked_on : nullptr;
                    if (!blk) break;
                    auto* next_lock = static_cast<rtmutex*>(blk);

                    if (next_lock != held_lock) {
                        next_lock->m_pi_lock.lock();
                    }
                    strand* next_owner = next_lock->m_owner;
                    const u8 tp = top_waiter_prio(next_lock->m_waiters);
                    if (next_owner) {
                        const strand* top = nullptr;
                        if (!next_lock->m_waiters.empty()) {
                            top = detail::strand_from_pi_waiters(*next_lock->m_waiters.begin());
                        }
                        if (top) reboost_owner(*next_owner, tp, *top);
                    }
                    if (held_lock != next_lock && held_lock != this) {
                        held_lock->m_pi_lock.unlock();
                    }
                    held_lock = next_lock;
                    if (!next_owner || next_owner == cur) break;
                    cur = next_owner;
                }
                if (held_lock != this) held_lock->m_pi_lock.unlock();
            }

            m_lock_word.store(0U, memory_order::release);
        }

        if (new_owner) {
            zxfoundation::sched::rt::task_wake_pi(*new_owner);
        }
    }

    [[nodiscard]] auto rtmutex::is_locked() const noexcept -> bool {
        return m_lock_word.load(memory_order::relaxed) != 0U;
    }
}
/// @}

/// @name private class rtmutex
/// @{
namespace zxfoundation::sync::rtmutex {

    auto rtmutex::slow_lock(const irqflags flags) noexcept -> void {
        using detail::effective_rt_prio;
        using detail::pi_may_cross;
        using detail::top_waiter_prio;
        using detail::reboost_owner;
        using detail::MAX_PI_DEPTH;

        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto* self = reinterpret_cast<strand*>(pcp.current_strand);

        for (;;) {
            {
                sync::irq_lock_guard pi_guard(m_pi_lock);

                u32 expected = 0U;
                if (m_lock_word.compare_exchange_strong(
                        expected, 1U, memory_order::acquire, memory_order::relaxed)) {
                    m_owner = self;
                    arch::s390x::cpu::irq::irq_restore(flags);
                    return;
                }

                if (m_owner == self) {
                    arch::s390x::cpu::irq::irq_restore(flags);
                    return;
                }

                if (self->body->blocked_on != this) {
                    self->body->blocked_on = this;
                    const u8 my_prio = effective_rt_prio(*self);
                    bool inserted = false;
                    lib::list_node* before_node = nullptr;
                    for (const auto m_waiter : m_waiters) {
                        before_node = m_waiter;
                        auto* ws = detail::strand_from_pi_waiters(before_node);
                        if (effective_rt_prio(*ws) < my_prio) {
                            m_waiters.insert_before(*before_node, self->body->pi_waiters);
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        m_waiters.push_back(self->body->pi_waiters);
                    }
                }

                strand* cur = m_owner;
                auto held_lock = this; // we already hold this->m_pi_lock.
                const strand* waiter_for_gate = self;
                u8 propagate_prio = effective_rt_prio(*self);
                for (u32 depth = 0U; depth < MAX_PI_DEPTH; ++depth) {
                    if (!cur) break;
                    if (!pi_may_cross(*waiter_for_gate, *cur)) break;
                    zxfoundation::sched::rt::boost_prio(*cur, propagate_prio);
                    void* blk = cur->body ? cur->body->blocked_on : nullptr;
                    if (!blk) break;
                    auto* next_lock = static_cast<rtmutex*>(blk);
                    if (next_lock != held_lock) {
                        next_lock->m_pi_lock.lock();
                    }
                    strand* next_owner = next_lock->m_owner;
                    const u8 next_top = top_waiter_prio(next_lock->m_waiters);
                    if (next_top > propagate_prio) propagate_prio = next_top;
                    waiter_for_gate = cur;
                    if (held_lock != this) held_lock->m_pi_lock.unlock();
                    held_lock = next_lock;
                    if (!next_owner || next_owner == cur) break;
                    cur = next_owner;
                }
                if (held_lock != this) held_lock->m_pi_lock.unlock();
            }

            if (self->on_rq) {
                zxfoundation::sched::dequeue_strand(self);
            }
            self->substate = sched_substate::blocked;
            zxfoundation::sched::schedule();

            sync::irq_lock_guard pi_guard(m_pi_lock);
            if (m_owner == self) {
                arch::s390x::cpu::irq::irq_restore(flags);
                return;
            }
        }
    }

}
/// @}