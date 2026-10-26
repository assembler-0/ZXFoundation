/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/rt.zcomponent/bandwidth.cxx
/// @brief Global RT bandwidth controller — sched_rt_period_us + per-CPU runtime
///        enforcement.

module zxfoundation.sched.rt.bandwidth;
import zxfoundation.sys.timer.types;
import zxfoundation.sys.timer.core;
import zxfoundation.sys.time.core;
import arch.s390x.cpu.clock;
import zxfoundation.base.types;
import zxfoundation.base.config;
import lib.error;
import lib.stl;

namespace zxfoundation::sched::rt::bw {

    /// @brief Period replenishment: reset the per-CPU RT runtime counter.
    /// @param[in,out] bw Control block (int).
    auto bw_replenish(rt_bandwidth& bw) noexcept -> void {
        bw.rt_time_ns = 0U;
        bw.rt_throttled = false;
    }

    /// @brief Initialize one per-CPU RT bandwidth control block.
    /// @param[in,out] bw   Control block to initialise (on BSP stack).
    /// @param[in] cpu      CPU id.
    auto bw_init(rt_bandwidth& bw, u16 cpu) noexcept -> void {
        bw.rt_period_ns = RT_PERIOD_DEFAULT_NS;
        bw.rt_runtime_ns = RT_RUNTIME_DEFAULT_NS;
        bw.rt_time_ns = 0U;
        bw.rt_period_start_ns = zxfoundation::sys::time::monotonic_now().nanoseconds_since_epoch();
        bw.rt_throttled = false;
        bw.rt_period_timer.deadline_tod =
            arch::s390x::cpu::clock::stck()
            + sys::timer::ns_to_tod(RT_PERIOD_DEFAULT_NS);
        bw.rt_period_timer.interval_tod = sys::timer::ns_to_tod(RT_PERIOD_DEFAULT_NS);
        bw.rt_period_timer.callback = [](void* data) noexcept -> void {
            auto* const p_bw = static_cast<rt_bandwidth*>(data);
            if (!p_bw) return;
            bw_replenish(*p_bw);
            const u64 now_ns = zxfoundation::sys::time::monotonic_now().nanoseconds_since_epoch();
            discard_value now_ns;  // record available for future analytics
        };
        bw.rt_period_timer.data = &bw;
        bw.rt_period_timer.mode = zxfoundation::sys::timer::timer_mode::periodic;
        bw.rt_period_timer.armed = false;
        bw.rt_period_timer.cpu = cpu;
        discard_value zxfoundation::sys::timer::timer_add(bw.rt_period_timer);
    }

    /// @brief Assess the RT bandwidth for the current CPU
    /// @param[in,out] bw   Control block (mutated to set throttled).
    /// @param[in]     consumed_ns RT consumption accounting delta (1 tick).
    /// @return True when the CPU is now throttled.
    auto bw_charge(rt_bandwidth& bw, u64 consumed_ns) noexcept -> bool {
        bw.rt_time_ns += consumed_ns;
        if (bw.rt_time_ns >= bw.rt_runtime_ns) {
            bw.rt_throttled = true;
            return true;
        }
        return false;
    }

} // namespace zxfoundation::sched::rt::bw
