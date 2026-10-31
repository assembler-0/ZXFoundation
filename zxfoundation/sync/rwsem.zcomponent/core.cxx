/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sync/rwsem.zcomponent/core.cxx
/// @brief Turnstile-backed writer-preferring reader/writer semaphore.

module zxfoundation.sync.rwsem.core;
import zxfoundation.sync.rwsem.types;
import zxfoundation.sync.turnstile;
import zxfoundation.sched.context.core;
import zxfoundation.sched.core;
import zxfoundation.sched.policy;
import zxfoundation.dgp.domain.types;
import arch.s390x.cpu.percpu;
import lib.container_of;

namespace zxfoundation::sync::rwsem {

    using zxfoundation::dgp::domain::domain;
    using zxfoundation::dgp::domain::strand;
    using zxfoundation::sched::sched_substate;

    static auto wake_waiter(void* opaque) noexcept -> void {
        auto* waiter_ptr = static_cast<waiter*>(opaque);
        if (!waiter_ptr || !waiter_ptr->strand_ptr) return;
        zxfoundation::sched::wake_strand(*waiter_ptr->strand_ptr);
    }

    auto rw_semaphore::lock_shared() noexcept -> void {
        zxfoundation::sched::context::assert_can_sleep();
        auto* self = reinterpret_cast<strand*>(arch::s390x::cpu::percpu::this_cpu().current_strand);
        waiter current{}; current.strand_ptr = self; current.writer = false;
        current.node.context = &current; current.node.wake_fn = wake_waiter;
        for (;;) {
            const auto flags = m_turnstile.lock_irqsave();
            if (current.node.state.load(std::memory_order::acquire) == turnstile::waiter_state::granted) {
                m_turnstile.unlock_irqrestore(flags); return;
            }
            if (!m_writer && m_waiting_writers == 0U) { ++m_readers; m_turnstile.unlock_irqrestore(flags); return; }
            m_turnstile.enqueue_locked(current.node); m_turnstile.unlock_irqrestore(flags);
            self->substate = sched_substate::blocked; zxfoundation::sched::schedule();
        }
    }

    auto rw_semaphore::lock() noexcept -> void {
        zxfoundation::sched::context::assert_can_sleep();
        auto* self = reinterpret_cast<strand*>(arch::s390x::cpu::percpu::this_cpu().current_strand);
        waiter current{}; current.strand_ptr = self; current.writer = true;
        current.node.context = &current; current.node.wake_fn = wake_waiter;
        for (;;) {
            const auto flags = m_turnstile.lock_irqsave();
            if (current.node.state.load(std::memory_order::acquire) == turnstile::waiter_state::granted) {
                m_turnstile.unlock_irqrestore(flags); return;
            }
            if (!m_writer && m_readers == 0U) { m_writer = true; m_turnstile.unlock_irqrestore(flags); return; }
            if (current.node.state.load(std::memory_order::acquire) != turnstile::waiter_state::queued) {
                m_turnstile.enqueue_locked(current.node); ++m_waiting_writers;
            }
            m_turnstile.unlock_irqrestore(flags);
            self->substate = sched_substate::blocked; zxfoundation::sched::schedule();
        }
    }

    [[nodiscard]] auto rw_semaphore::try_lock_shared() noexcept -> bool {
        const auto flags = m_turnstile.lock_irqsave();
        const bool acquired = !m_writer && m_waiting_writers == 0U; if (acquired) ++m_readers;
        m_turnstile.unlock_irqrestore(flags); return acquired;
    }

    [[nodiscard]] auto rw_semaphore::try_lock() noexcept -> bool {
        const auto flags = m_turnstile.lock_irqsave();
        const bool acquired = !m_writer && m_readers == 0U; if (acquired) m_writer = true;
        m_turnstile.unlock_irqrestore(flags); return acquired;
    }

    auto rw_semaphore::unlock_shared() noexcept -> void {
        waiter* wake = nullptr; const auto flags = m_turnstile.lock_irqsave();
        if (m_readers != 0U) --m_readers;
        if (m_readers == 0U && !m_writer) {
            auto* candidate = m_turnstile.first_locked();
            while (candidate && !lib::container_of<waiter, turnstile::waiter,
                    &waiter::node>(candidate)->writer)
                candidate = m_turnstile.next_locked(*candidate);
            if (candidate) {
                m_turnstile.grant_specific_locked(*candidate);
                wake = lib::container_of<waiter, turnstile::waiter, &waiter::node>(candidate);
                --m_waiting_writers; m_writer = true;
            }
        }
        m_turnstile.unlock_irqrestore(flags);
        if (wake && wake->node.wake_fn) wake->node.wake_fn(wake->node.context);
    }

    auto rw_semaphore::unlock() noexcept -> void {
        lib::list_head wake_list{};
        const auto flags = m_turnstile.lock_irqsave(); m_writer = false;
        auto* candidate = m_turnstile.first_locked();
        auto* writer_candidate = candidate;
        while (writer_candidate && !lib::container_of<waiter, turnstile::waiter,
                &waiter::node>(writer_candidate)->writer)
            writer_candidate = m_turnstile.next_locked(*writer_candidate);
        if (writer_candidate) {
            m_turnstile.grant_specific_locked(*writer_candidate);
            wake_list.push_back(writer_candidate->link);
            --m_waiting_writers; m_writer = true;
        } else if (m_waiting_writers == 0U) {
            while (candidate && !lib::container_of<waiter,
                    turnstile::waiter, &waiter::node>(candidate)->writer) {
                auto* next = m_turnstile.next_locked(*candidate);
                m_turnstile.grant_specific_locked(*candidate);
                wake_list.push_back(candidate->link);
                ++m_readers; candidate = next;
            }
        }
        m_turnstile.unlock_irqrestore(flags);
        while (!wake_list.empty()) {
            auto* node = wake_list.pop_front();
            auto* turnstile_waiter = lib::container_of<turnstile::waiter,
                lib::list_node, &turnstile::waiter::link>(node);
            auto* wake = lib::container_of<waiter, turnstile::waiter,
                &waiter::node>(turnstile_waiter);
            if (wake->node.wake_fn) wake->node.wake_fn(wake->node.context);
        }
    }

    [[nodiscard]] auto rw_semaphore::is_write_locked() const noexcept -> bool {
        auto& turnstile = const_cast<turnstile::turnstile&>(m_turnstile);
        const auto flags = turnstile.lock_irqsave(); const bool result = m_writer;
        turnstile.unlock_irqrestore(flags); return result;
    }

}
