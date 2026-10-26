/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/signal.zcomponent/core.cxx
/// @brief Signal delivery implementation

module zxfoundation.dgp.signal.core;
import zxfoundation.dgp.signal.types;
import zxfoundation.dgp.domain.types;
import zxfoundation.base.types;
import zxfoundation.sys.printk.core;
import zxfoundation.base.typestate;
import zxfoundation.scoms.kobject.ref;
import lib.error;
import lib.stl;

namespace zxfoundation::dgp::signal {

    using namespace zxfoundation::sys::printk;
    using zxfoundation::dgp::domain::domain;
    using zxfoundation::dgp::domain::domain_flags;

    /// @brief Inner: dispatch a dequeued entry to its registered handler,
    ///        falling back to its signal_default action when no handler is
    ///        installed.
    [[nodiscard]] auto deliver_entry(domain& d, signal_entry entry) noexcept -> bool {
        if (entry.kind == signal_kind::none) {
            return false;
        }
        const u8 idx = static_cast<u8>(entry.kind);
        const auto& hinfo = d.signal_queue.handlers[idx];

        bool consumed = false;
        if (hinfo.handler != nullptr) {
            auto* s = &d.primary_strand;
            if (s->body != nullptr && s->body->signal_return_kind == 0U) {
                s->body->signal_return_kind   = static_cast<u8>(entry.kind);
                s->body->signal_handler_va    = reinterpret_cast<u64>(hinfo.handler);
                s->body->signal_payload       = entry.data;
                s->body->signal_user_ctx      = reinterpret_cast<u64>(hinfo.user_data);
                consumed = true;
            }
            if (consumed) return true;
        }

        {
            auto effective_def = hinfo.def;
            if (hinfo.handler == nullptr) {
                switch (entry.kind) {
                    case signal_kind::fault:
                    case signal_kind::term:
                        effective_def = signal_default::terminate;
                        break;
                    default:
                        break;
                }
            }
            switch (effective_def) {
                case signal_default::terminate:
                    pinfo("signal: domain {} — unhandled {} (default: terminate)",
                          d.ko.id, signal_kind_name(entry.kind));
                    discard_value scoms::kobject::mark_dying(&d);
                    d.flags = d.flags | domain_flags::need_resched;
                    return true;    // consumed: the domain is dying
                case signal_default::notify:
                    // Stays queued: the domain polls pending_mask and acks.
                    return false;
                case signal_default::ignore:
                default:
                    return false;
            }
        }
        return false;
    }
        
    /// @brief Register a signal handler for the given domain and kind.
    /// @param[in,out] d     The target domain.
    /// @param[in]     kind  The signal kind to handle.
    /// @param[in]     info  Handler descriptor (function, user data, default).
    /// @return  Success, or kernel_error if kind is out of range.
    [[nodiscard]] auto signal_register_handler(
        domain&               d,
        signal_kind           kind,
        const signal_handler_info& info
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const u8 idx = static_cast<u8>(kind);
        if (idx >= 8) {
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        d.signal_queue.handlers[idx] = info;
        return {};
    }

    /// @brief Queue a signal for asynchronous delivery to a domain.
    /// @param[in,out] d     The target domain.
    /// @param[in]     kind  Signal kind.
    /// @param[in]     data  Opaque payload.
    auto signal_enqueue(
        domain&     d,
        signal_kind kind,
        u64         data
    ) noexcept -> void {
        d.signal_queue.enqueue(kind, data);
    }

    /// @brief Update the per-domain blocked mask.
    auto signal_block(domain& d, u32 mask) noexcept -> void {
        d.signal_queue.blocked_mask = mask;
    }

    /// @brief Return the current blocked mask.
    [[nodiscard]] auto signal_blocked_mask(const domain& d) noexcept -> u32 {
        return d.signal_queue.blocked_mask;
    }

    /// @brief Deliver one unblocked pending signal to the domain.
    /// @return  True if a signal was delivered; false if the queue was empty
    ///          or every pending kind was blocked.
    auto signal_deliver_one(domain& d) noexcept -> bool {
        auto& q = d.signal_queue;
        if (q.count == 0) return false;
        for (u32 i = 0U; i < q.count; ++i) {
            const u32 idx = (q.head + i) % SIGNAL_QUEUE_DEPTH;
            const auto k = q.entries[idx].kind;
            if (k == signal_kind::none) continue;
            const u32 kbit = 1U << static_cast<u8>(k);
            if ((q.blocked_mask & kbit) == 0U) {
                const auto entry = q.entries[idx];
                if (!deliver_entry(d, entry)) {
                    continue;
                }
                if (idx == q.head) {
                    q.entries[q.head] = {};
                    q.head = (q.head + 1) % SIGNAL_QUEUE_DEPTH;
                } else {
                    for (u32 j = idx;
                         j != (q.head + (q.count - 1U)) % SIGNAL_QUEUE_DEPTH;
                         j = (j + 1) % SIGNAL_QUEUE_DEPTH) {
                        q.entries[j] = q.entries[(j + 1U) % SIGNAL_QUEUE_DEPTH];
                    }
                    const u32 last = (q.head + (q.count - 1U)) % SIGNAL_QUEUE_DEPTH;
                    q.entries[last] = {};
                }
                --q.count;
                if (q.count == 0) q.pending_mask = 0;
                else {
                    q.pending_mask = 0;
                    for (u32 r = 0U; r < q.count; ++r) {
                        const auto rk = q.entries[(q.head + r) % SIGNAL_QUEUE_DEPTH].kind;
                        if (rk != signal_kind::none)
                            q.pending_mask |= (1U << static_cast<u8>(rk));
                    }
                }
                return true;
            }
        }
        return false;
    }

    /// @brief Consume queued signals whose kind bits appear in ack_mask.
    /// @param[in,out] d        The target domain.
    /// @param[in]     ack_mask Bitmask of signal_kind values acknowledged.
    /// @note  Matching ring entries are dequeued and the pending mask is
    ///        rebuilt from the surviving entries
    auto signal_ack(domain& d, u32 ack_mask) noexcept -> void {
        auto& q = d.signal_queue;
        u32 write = q.head;
        u32 kept  = 0U;
        for (u32 i = 0U; i < q.count; ++i) {
            const u32 idx = (q.head + i) % SIGNAL_QUEUE_DEPTH;
            const auto e = q.entries[idx];
            q.entries[idx] = {};
            const u32 kbit = 1U << static_cast<u8>(e.kind);
            if (e.kind == signal_kind::none || (ack_mask & kbit) != 0U) {
                continue;
            }
            q.entries[write] = e;
            write = (write + 1) % SIGNAL_QUEUE_DEPTH;
            ++kept;
        }
        q.tail = write;
        q.count = kept;
        q.pending_mask = 0;
        for (u32 r = 0U; r < kept; ++r) {
            const auto rk = q.entries[(q.head + r) % SIGNAL_QUEUE_DEPTH].kind;
            if (rk != signal_kind::none)
                q.pending_mask |= (1U << static_cast<u8>(rk));
        }
    }

    /// @brief Deliver all pending signals to the domain.
    /// @return  true if the domain should be rescheduled (terminate default
    ///          was taken).
    auto signal_deliver_all(domain& d) noexcept -> bool {
        bool needs_resched = false;
        while (d.signal_queue.has_pending()) {
            if (!signal_deliver_one(d)) {
                break;
            }
            if (d.ko.state == base::lifecycle_state::dying) {
                needs_resched = true;
                break;
            }
        }
        return needs_resched;
    }


} // namespace zxfoundation::dgp::signal
