/// SPDX-License-Identifier: Apache 2.0
/// @file zxfoundation/sys/time.cxxm
/// @brief High-performance industrial kernel time system for s390x.

module zxfoundation.sys.time.core;
import zxfoundation.base.types;
import zxfoundation.sync.seqlock;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.clock;
import arch.s390x.cpu.irq;
import lib.error;
import std;

namespace zxfoundation::sys::time::detail {

    /// @brief Seqlock-protected wall-clock offset.
    no_export zxfoundation::sync::seqlock<u64> g_wall_offset{};

    /// @brief TOD value captured at kernel entry (set once by time::initialize).
    no_export u64 g_boot_tod{0};

    /// @brief Monotonic tick counter — incremented by the clock comparator handler.
    no_export std::atomic<u64> g_tick{0};

}

namespace zxfoundation::sys::time {

    /// @brief Initialize the global time system.
    /// @param[in] tod_boot  TOD clock value captured at kernel entry by ZXFL.
    auto initialize(const u64 tod_boot) noexcept -> void {
        detail::g_boot_tod = tod_boot;
        detail::g_wall_offset.write(0ULL);   // no IRQ disable needed before EXT is armed
    }

    /// @brief Retrieve the current monotonic time (time since boot).
    [[nodiscard]] auto monotonic_now() noexcept -> instant {
        const u64 now   = arch::s390x::cpu::clock::stck();
        const u64 delta = (now > detail::g_boot_tod)
                        ? (now - detail::g_boot_tod) : 0U;
        return instant(duration::from_tod(delta).count_ns());
    }

    /// @brief Retrieve the current realtime (wall-clock) time.
    [[nodiscard]] auto realtime_now() noexcept -> instant {
        const u64 raw_ns = duration::from_tod(
            arch::s390x::cpu::clock::stck()).count_ns();
        const u64 offset = detail::g_wall_offset.read();
        return instant(raw_ns + offset);
    }

    /// @brief Adjust the realtime clock (e.g., NTP steering).
    /// @param[in]  now  New absolute epoch-relative time.
    [[nodiscard]] auto set_realtime(const instant now) noexcept
        -> std::expected<void, lib::kernel_error>
    {
        const u64 raw_ns = duration::from_tod(
            arch::s390x::cpu::clock::stck()).count_ns();
        const u64 new_offset = now.nanoseconds_since_epoch() - raw_ns;

        const auto flags = arch::s390x::cpu::irq::irq_save_disable();
        detail::g_wall_offset.write(new_offset);
        arch::s390x::cpu::irq::irq_restore(flags);

        return {};
    }

    /// @brief Advance the wall-clock offset by one tick's worth of time.
    /// @param[in]  tick_ns  Duration of one tick in nanoseconds (10,000,000).
    auto tick_advance_wall(u64 tick_ns) noexcept -> void {
        discard_value detail::g_tick.fetch_add(
            1U, std::memory_order::relaxed);
        const u64 old_offset = detail::g_wall_offset.read();
        detail::g_wall_offset.write(old_offset + tick_ns);
    }

    /// @brief Return the current monotonic tick counter.
    [[nodiscard]] auto tick_count() noexcept -> u64 {
        return detail::g_tick.load(std::memory_order::relaxed);
    }

} // namespace zxfoundation::sys::time
