/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sync/turnstile.zcomponent/core.cxx
/// @brief Shared ordered waiter substrate for sleepable synchronization.

module zxfoundation.sync.turnstile.core;
import zxfoundation.sync.turnstile.types;
import zxfoundation.sync.lockable;
import arch.s390x.cpu.irq;
import lib.container_of;

namespace zxfoundation::sync::turnstile {

    auto turnstile::lock_irqsave() noexcept -> ::irqflags {
        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        m_lock.lock();
        return flags;
    }

    auto turnstile::unlock_irqrestore(const ::irqflags flags) noexcept -> void {
        m_lock.unlock();
        arch::s390x::cpu::irq::irq_restore(flags);
    }

    auto turnstile::enqueue_locked(waiter& w) noexcept -> bool {
        const auto state = w.state.load(std::memory_order::acquire);
        if (state == waiter_state::queued) return true;
        if (state == waiter_state::granted) return false;
        w.state.store(waiter_state::queued, std::memory_order::release);
        for (auto* node : m_waiters) {
            auto* other = lib::container_of<waiter, lib::list_node, &waiter::link>(node);
            if (other->priority < w.priority) {
                m_waiters.insert_before(*node, w.link);
                return true;
            }
        }
        m_waiters.push_back(w.link);
        return true;
    }

    auto turnstile::cancel_locked(waiter& w) noexcept -> bool {
        if (w.state.load(std::memory_order::acquire) != waiter_state::queued)
            return false;
        lib::list_head::remove(w.link);
        w.state.store(waiter_state::cancelled, std::memory_order::release);
        return true;
    }

    auto turnstile::grant_locked() noexcept -> waiter* {
        if (m_waiters.empty()) return nullptr;
        auto* node = m_waiters.pop_front();
        auto* w = lib::container_of<waiter, lib::list_node, &waiter::link>(node);
        w->state.store(waiter_state::granted, std::memory_order::release);
        return w;
    }

    auto turnstile::first_locked() noexcept -> waiter* {
        if (m_waiters.empty()) return nullptr;
        return lib::container_of<waiter, lib::list_node, &waiter::link>(m_waiters.sentinel.next);
    }

    auto turnstile::next_locked(waiter& w) noexcept -> waiter* {
        auto* node = w.link.next;
        if (node == &m_waiters.sentinel) return nullptr;
        return lib::container_of<waiter, lib::list_node, &waiter::link>(node);
    }

    auto turnstile::grant_specific_locked(waiter& w) noexcept -> bool {
        if (w.state.load(std::memory_order::acquire) != waiter_state::queued)
            return false;
        lib::list_head::remove(w.link);
        w.state.store(waiter_state::granted, std::memory_order::release);
        return true;
    }

    [[nodiscard]] auto turnstile::empty_locked() const noexcept -> bool {
        return m_waiters.empty();
    }

    auto turnstile::enqueue(waiter& w) noexcept -> bool {
        const auto flags = lock_irqsave();
        const bool result = enqueue_locked(w);
        unlock_irqrestore(flags);
        return result;
    }

    auto turnstile::cancel(waiter& w) noexcept -> bool {
        const auto flags = lock_irqsave();
        const bool result = cancel_locked(w);
        unlock_irqrestore(flags);
        return result;
    }

    auto turnstile::wake_one() noexcept -> bool {
        waiter* w = nullptr;
        const auto flags = lock_irqsave();
        w = grant_locked();
        unlock_irqrestore(flags);
        if (!w) return false;
        if (w->wake_fn) w->wake_fn(w->context);
        return true;
    }

    [[nodiscard]] auto turnstile::count() const noexcept -> u32 {
        auto& self = const_cast<turnstile&>(*this);
        const auto flags = self.lock_irqsave();
        const auto result = static_cast<u32>(self.m_waiters.count());
        self.unlock_irqrestore(flags);
        return result;
    }

}
