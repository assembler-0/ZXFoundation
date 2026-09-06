/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/cbs.cxx
/// @brief Per-strand CBS (Constant Bandwidth Server) replenishment timers
///        for the EDF deadline scheduling class

module zxfoundation.sched.cbs.core;
import zxfoundation.sched.entity;
import zxfoundation.sys.timer.types;
import zxfoundation.sys.timer.core;
import zxfoundation.sys.time.core;
import zxfoundation.base.types;
import lib.error;
import lib.stl;

namespace zxfoundation::sched::cbs {

    /// @brief Build a CBS block for a strand that is about to be admitted.
    /// @param[out] cbs  Output record (stack-resident).
    auto init(cbs_replen& cbs) noexcept -> void {
        cbs.period_ns = CBS_PERIOD_DEFAULT_NS;
        cbs.runtime_ns = CBS_RUNTIME_DEFAULT_NS;
        cbs.used_runtime_ns = 0U;
        cbs.period_deadline = 0ULL;
        cbs.timer.deadline_tod = 0ULL;
        cbs.timer.interval_tod = 0ULL;
        cbs.timer.callback = nullptr;
        cbs.timer.data = nullptr;
        cbs.timer.mode = zxfoundation::sys::timer::timer_mode::oneshot;
        cbs.timer.armed = false;
        cbs.timer.cpu = 0U;
    }

    /// @brief Charge one tick against the CBS runtime.
    /// @param[in,out] cbs       Local CBS block.
    /// @param[in]     delta_ns  Charge amount (1 ms = 1 tick accounting).
    /// @return true when the runtime is exhausted and throttling should
    ///         occur (the strand should be dequeued).
    auto charge(cbs_replen& cbs, u64 delta_ns) noexcept -> bool {
        cbs.used_runtime_ns += delta_ns;
        return (cbs.used_runtime_ns >= cbs.runtime_ns);
    }

    /// @brief Replenish a CBS record with the remainder of the current period.
    /// @note Called when the period timer fires; the strand is re‑enqueued
    ///       when it was throttleing.
    auto replenish(cbs_replen& cbs) noexcept -> void {
        const u64 now = zxfoundation::sys::time::monotonic_now().nanoseconds_since_epoch();
        cbs.period_deadline = now + cbs.period_ns;
        cbs.used_runtime_ns = 0ULL;
    }

    /// @brief Install the CBS timer expiry handler on a strand's CBS block.
    /// @param[in,out] cbs     CBS block (overwrites timer members).
    /// @param[in]     cpu     CPU id the timer is pinned to.
    /// @param[in]     cb      Callback function pointer (must not be null).
    /// @param[in]     data    Opaque callback data (typically the strand*).
    auto install_timer(cbs_replen& cbs, u16 cpu,
                            zxfoundation::sys::timer::timer_fn cb,
                            void* data) noexcept -> void {
        cbs.timer.deadline_tod = cbs.period_deadline;
        cbs.timer.interval_tod = cbs.period_ns;
        cbs.timer.callback = cb;
        cbs.timer.data = data;
        cbs.timer.mode = zxfoundation::sys::timer::timer_mode::oneshot;
        cbs.timer.armed = true;
        cbs.timer.cpu = cpu;
    }

    /// @brief Remove CBS timer from the timer heap (called on dequeue/destroy).
    auto cancel_timer(cbs_replen& cbs) noexcept -> void {
        if (cbs.timer.armed) {
            discard_value zxfoundation::sys::timer::timer_cancel(cbs.timer);
            cbs.timer.armed = false;
        }
    }

} // namespace zxfoundation::sched::cbs