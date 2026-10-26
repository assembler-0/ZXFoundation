/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sync/waitqueue.zcomponent/core.cxx
/// @brief Wait queue operations — the FIFO flavor over the turnstile
///        substrate (ch. 20 §20.2a).

module zxfoundation.sync.waitqueue.core;
import zxfoundation.sync.waitqueue.types;
import zxfoundation.sync.turnstile;
import zxfoundation.base.types;
import arch.s390x.cpu.lowcore;
import lib.container_of;
import lib.stl;

namespace zxfoundation::sync::waitqueue {

    using turnstile::waiter_state;

    auto init(wait_head& wq) noexcept -> void {
        static_cast<void>(wq);
    }

    auto prepare_to_wait(wait_head& wq,
                         wait_entry& entry,
                         wait_flags flags) noexcept -> void {
        const auto* lc = arch::s390x::cpu::lowcore::get_current();
        const u64 dom_addr = lc->get<"current_domain">();
        u32 dom_id = 0xFFFFFFFFU;
        if (dom_addr != 0) {
            dom_id = *reinterpret_cast<const u32*>(dom_addr);
        }

        const auto saved_flags = wq.q.lock_irqsave();

        if (entry.node.state.load(std::memory_order::acquire) == waiter_state::queued) {
            entry.flags = flags;
            wq.q.unlock_irqrestore(saved_flags); return;
        }

        entry.flags     = flags;
        entry.result.store(wait_result::woken, std::memory_order::relaxed);
        entry.domain_id = dom_id;
        entry.node.priority = 0;
        discard_value wq.q.enqueue_locked(entry.node);
        wq.q.unlock_irqrestore(saved_flags);
    }

    auto finish_wait(wait_head& wq,
                     wait_entry& entry) noexcept -> void {
        const auto flags = wq.q.lock_irqsave();
        if (entry.node.state.load(std::memory_order::acquire) == waiter_state::queued) {
            discard_value wq.q.cancel_locked(entry.node);
        }
        wq.q.unlock_irqrestore(flags);
    }

    auto wake_up(wait_head& wq) noexcept -> void {
        bool exclusive_woken = false;
        for (;;) {
            wait_entry* entry = nullptr;
            {
                const auto flags = wq.q.lock_irqsave();
                for (auto* node = wq.q.first_locked(); node; node = wq.q.next_locked(*node)) {
                    auto* candidate = lib::container_of<wait_entry, turnstile::waiter,
                        &wait_entry::node>(node);
                    const bool exclusive =
                        (candidate->flags & wait_flags::exclusive) != wait_flags::none;
                    if (exclusive && exclusive_woken) continue;
                    entry = candidate;
                    entry->result.store(wait_result::woken, std::memory_order::release);
                    discard_value wq.q.grant_specific_locked(*node);
                    if (exclusive) exclusive_woken = true;
                    break;
                }
                wq.q.unlock_irqrestore(flags);
            }
            if (!entry) return;
            if (entry->node.wake_fn) entry->node.wake_fn(entry->node.context);
        }
    }

    auto wake_up_one(wait_head& wq) noexcept -> void {
        wait_entry* entry = nullptr;
        {
            const auto flags = wq.q.lock_irqsave();
            if (auto* node = wq.q.first_locked()) {
                entry = lib::container_of<wait_entry, turnstile::waiter,
                    &wait_entry::node>(node);
                entry->result.store(wait_result::woken, std::memory_order::release);
                discard_value wq.q.grant_specific_locked(*node);
            }
            wq.q.unlock_irqrestore(flags);
        }
        if (entry && entry->node.wake_fn) entry->node.wake_fn(entry->node.context);
    }

    [[nodiscard]] auto active(const wait_head& wq) noexcept -> bool {
        auto& q = const_cast<turnstile::turnstile&>(wq.q);
        const auto flags = q.lock_irqsave();
        const bool result = !q.empty_locked();
        q.unlock_irqrestore(flags);
        return result;
    }

    [[nodiscard]] auto count(const wait_head& wq) noexcept -> u32 {
        return const_cast<turnstile::turnstile&>(wq.q).count();
    }

} // namespace zxfoundation::sync::waitqueue
