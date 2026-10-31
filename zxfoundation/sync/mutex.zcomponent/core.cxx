/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sync/mutex.zcomponent/core.cxx
/// @brief Turnstile-backed blocking mutex implementation.

module zxfoundation.sync.mutex.core;
import zxfoundation.sync.turnstile;
import zxfoundation.sched.context.core;
import zxfoundation.sched.core;
import zxfoundation.sched.policy;
import zxfoundation.dgp.domain.types;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.irq;
import lib.stl;

namespace zxfoundation::sync::mutex {

    using zxfoundation::dgp::domain::domain;
    using zxfoundation::dgp::domain::strand;
    using zxfoundation::sched::sched_substate;

    struct mutex_waiter {
        turnstile::waiter node{};
        strand* strand_ptr{nullptr};
    };

    static auto wake_mutex_waiter(void* opaque) noexcept -> void {
        auto* waiter = static_cast<mutex_waiter*>(opaque);
        if (!waiter || !waiter->strand_ptr) return;
        zxfoundation::sched::wake_strand(*waiter->strand_ptr);
    }

    auto mutex::lock() noexcept -> void {
        zxfoundation::sched::context::assert_can_sleep();
        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto* self = reinterpret_cast<strand*>(pcp.current_strand);
        u32 expected = 0U;
        if (m_val.compare_exchange_strong(expected, 1U,
                std::memory_order::acquire, std::memory_order::relaxed)) {
            m_owner = self;
            arch::s390x::cpu::irq::irq_restore(flags);
            return;
        }
        slow_lock(flags);
    }

    [[nodiscard]] auto mutex::try_lock() noexcept -> bool {
        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto* self = reinterpret_cast<strand*>(pcp.current_strand);
        u32 expected = 0U;
        const bool acquired = m_val.compare_exchange_strong(expected, 1U,
            std::memory_order::acquire, std::memory_order::relaxed);
        if (acquired) m_owner = self;
        arch::s390x::cpu::irq::irq_restore(flags);
        return acquired;
    }

    auto mutex::unlock() noexcept -> void {
        mutex_waiter* selected = nullptr;
        const auto flags = m_turnstile.lock_irqsave();
        auto* current = reinterpret_cast<strand*>(
            arch::s390x::cpu::percpu::this_cpu().current_strand);
        if (m_owner != current) [[unlikely]] {
            m_turnstile.unlock_irqrestore(flags);
            __builtin_trap();
        }
        selected = reinterpret_cast<mutex_waiter*>(m_turnstile.grant_locked());
        if (selected) {
            m_owner = selected->strand_ptr;
            m_val.store(1U, std::memory_order::release);
        } else {
            m_owner = nullptr;
            m_val.store(0U, std::memory_order::release);
        }
        m_turnstile.unlock_irqrestore(flags);
        if (selected && selected->node.wake_fn)
            selected->node.wake_fn(selected->node.context);
    }

    [[nodiscard]] auto mutex::is_locked() const noexcept -> bool {
        return m_val.load(std::memory_order::relaxed) != 0U;
    }

    auto mutex::slow_lock(const irqflags flags) noexcept -> void {
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto* self = reinterpret_cast<strand*>(pcp.current_strand);
        mutex_waiter waiter{};
        waiter.strand_ptr = self;
        waiter.node.context = &waiter;
        waiter.node.wake_fn = wake_mutex_waiter;
        for (;;) {
            const auto queue_flags = m_turnstile.lock_irqsave();
            if (waiter.node.state.load(std::memory_order::acquire)
                    == turnstile::waiter_state::granted) {
                m_turnstile.unlock_irqrestore(queue_flags);
                arch::s390x::cpu::irq::irq_restore(flags);
                return;
            }
            u32 expected = 0U;
            if (m_val.compare_exchange_strong(expected, 1U,
                    std::memory_order::acquire, std::memory_order::relaxed)) {
                m_owner = self;
                m_turnstile.cancel_locked(waiter.node);
                m_turnstile.unlock_irqrestore(queue_flags);
                arch::s390x::cpu::irq::irq_restore(flags);
                return;
            }
            m_turnstile.enqueue_locked(waiter.node);
            m_turnstile.unlock_irqrestore(queue_flags);
            self->substate = sched_substate::blocked;
            zxfoundation::sched::schedule();
        }
    }

}
