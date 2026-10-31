/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sync/futex.zcomponent/core.cxx
/// @brief The user futex: sleep until a user-memory word changes.

module zxfoundation.sync.futex.core;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sys.printk.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.sync.waitqueue.core;
import zxfoundation.sync.waitqueue.types;
import zxfoundation.sched.core;
import zxfoundation.sched.policy;
import zxfoundation.sys.timer.core;
import zxfoundation.sys.time.core;
import zxfoundation.sys.zinit.core;
import zxfoundation.sys.timer.types;
import arch.s390x.cpu.clock;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.uaccess;
import lib.error;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace zxfoundation::sync::futex {

    namespace {
        constexpr u32 FUTEX_BUCKETS = 256U;

        struct bucket {
            zxfoundation::sync::waitqueue::wait_head wq{};
        };

        alignas(8) u8 g_buckets_raw[sizeof(bucket) * FUTEX_BUCKETS]{};

        auto buckets() noexcept -> bucket* {
            return reinterpret_cast<bucket*>(g_buckets_raw);
        }

        [[nodiscard]] auto bucket_for(u32 domain_id, u64 uaddr) noexcept -> bucket& {
            return buckets()[(domain_id ^ (uaddr >> 3U)) % FUTEX_BUCKETS];
        }

        auto strand_wake(void* opaque) noexcept -> void {
            auto* s = static_cast<zxfoundation::dgp::domain::strand*>(opaque);
            if (!s) return;
            zxfoundation::sched::wake_strand(*s);
        }

        struct timeout_ctx {
            zxfoundation::sync::waitqueue::wait_head* wq;
            bool fired;
        };

        auto timer_expiry(void* data) noexcept -> void {
            auto* ctx = static_cast<timeout_ctx*>(data);
            ctx->fired = true;
            zxfoundation::sync::waitqueue::wake_up(*ctx->wq);
        }
    }

    static auto zi_futex_init(const sys::zinit::zi_desc&) noexcept -> sys::zinit::zi_result {
        for (u32 i = 0; i < FUTEX_BUCKETS; ++i) {
            ::new(static_cast<void*>(&buckets()[i])) bucket{};
        }
        return {};
    }
    __zinit__(core, p00, zi_futex_init, fatal)

    auto futex_wait(
        zxfoundation::dgp::domain::domain& caller,
        u64 uaddr, u64 expected, u64 timeout_ns
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        auto* self = reinterpret_cast<zxfoundation::dgp::domain::strand*>(
            pcp.current_strand);
        if (!self) return std::unexpected(lib::kernel_error::from_generic(
            lib::generic_error::internal_error));

        auto& b = bucket_for(caller.ko.id, uaddr);

        // Fast path: the word already differs — never sleep.
        u64 value = 0;
        if (arch::s390x::cpu::uaccess::copy_from_user(
                &value, uaddr, sizeof(value), caller.root_phys) != 0) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        if (value != expected) return {};

        zxfoundation::sync::waitqueue::wait_entry entry{};
        entry.node.context = self;
        entry.node.wake_fn = strand_wake;

        sys::timer::kernel_timer timeout{};
        timeout_ctx ctx{ &b.wq, false };
        const bool timed = timeout_ns != 0ULL;
        if (timed) {
            timeout.callback = timer_expiry;
            timeout.data = &ctx;
            timeout.deadline_tod = arch::s390x::cpu::clock::stck()
                + sys::time::duration{timeout_ns}.to_tod();
            const auto armed = sys::timer::timer_add(timeout);
            if (!armed) return std::unexpected(armed.error());
        }

        self->substate = zxfoundation::sched::sched_substate::blocked;
        bool read_failed = false;
        for (;;) {
            if (zxfoundation::sync::waitqueue::prepare_to_wait_event(
                    b.wq, entry,
                    zxfoundation::sync::waitqueue::wait_flags::none,
                    [&ctx, &entry]() noexcept -> bool {
                        return ctx.fired
                            || entry.node.state.load(std::memory_order::acquire)
                                   == zxfoundation::sync::turnstile::waiter_state::granted;
                    }))
                break;
            if (arch::s390x::cpu::uaccess::copy_from_user(
                    &value, uaddr, sizeof(value), caller.root_phys) != 0) {
                read_failed = true;
                break;
            }
            if (value != expected) break;
            zxfoundation::sched::schedule();
        }
        self->substate = zxfoundation::sched::sched_substate::running;
        if (timed && !ctx.fired) discard_value sys::timer::timer_cancel(timeout);
        zxfoundation::sync::waitqueue::finish_wait(b.wq, entry);

        if (read_failed) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        if (ctx.fired) {
            return std::unexpected(lib::kernel_error::from_driver(
                lib::driver_error::timeout));
        }
        return {};
    }

    auto futex_wake(
        zxfoundation::dgp::domain::domain& caller,
        u64 uaddr, u64 count
    ) noexcept -> u32 {
        auto& b = bucket_for(caller.ko.id, uaddr);
        u32 woken = 0;
        for (u64 i = 0; i < count; ++i) {
            if (zxfoundation::sync::waitqueue::count(b.wq) == 0U)
                break;
            zxfoundation::sync::waitqueue::wake_up_one(b.wq);
            ++woken;
        }
        return woken;
    }

}
