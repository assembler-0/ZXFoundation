/// SPDX-License-Identifier: Apache-2.0
/// @file drivers/watchdog.zcomponent/core.cxx
/// @brief SMP watchdog — detects CPUs stuck without scheduler ticks.

module drivers.watchdog.core;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sys.timer.types;
import zxfoundation.sys.timer.core;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.zinit.core;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.diag;
import arch.s390x.cpu.clock;
import arch.s390x.cpu.psw;
import arch.s390x.trap.fixup.core;
import lib.error;
import lib.stl;

#define __zxmacro_want_zinit
#include macro_extensions

namespace drivers::watchdog::detail {

    using namespace zxfoundation::base::config;
    using namespace zxfoundation::sys::timer;
    using namespace zxfoundation::sys::printk;
    using namespace arch::s390x::cpu;

    /// @brief Arm / re-ping the hypervisor watchdog.
    static auto harddog_ping() noexcept -> bool {
        i32 rc = diag::diag288(diag::diag288_fn::change, diag::WDT_RESTART, diag::WDT_DEFAULT_TIMEOUT, 0);
        if (rc != 0) {
            rc = diag::diag288(diag::diag288_fn::init, diag::WDT_RESTART, diag::WDT_DEFAULT_TIMEOUT, 0);
        }
        return rc == 0;
    }

    static constexpr u16 WATCHDOG_INTERVAL_SECONDS = 5;

    static constexpr u64 WATCHDOG_INTERVAL_TOD =
        WATCHDOG_INTERVAL_SECONDS * 4096ULL * 1'000'000ULL;

    static constexpr u32 WATCHDOG_MISS_THRESHOLD = 2;

    static constexpr u32 WATCHDOG_FATAL_WARNINGS = 3;

    static constexpr u32 WATCHDOG_FATAL_THRESHOLD =
        WATCHDOG_MISS_THRESHOLD + WATCHDOG_FATAL_WARNINGS;

    static bool g_harddog_armed = false;
    static std::array<u64, CONFIG_ZX_MAX_CPUS> g_last_heartbeat{};
    static std::array<u32, CONFIG_ZX_MAX_CPUS> g_miss_count{};
    static kernel_timer g_watchdog_timer{};

    /// @brief Softdog timer callback — monitors only online APs, pings harddog.
    static auto watchdog_check(void*) noexcept -> void {
        arch::s390x::cpu::cpumask::for_each_online_cpu(
            [](u16 cpu16) noexcept -> void {
                // The BSP (id 0) runs the watchdog itself; skip self-monitoring.
                if (cpu16 == 0U) return;
                const u32 cpu = cpu16;

                auto* pcp = percpu::cpu_percpu(cpu16);
                if (!pcp) return;

                const u64 hb = pcp->watchdog_heartbeat;

                if (hb == g_last_heartbeat[cpu]) {
                    ++g_miss_count[cpu];
                    if (g_miss_count[cpu] >= WATCHDOG_FATAL_THRESHOLD) {
                        zxfoundation::sys::syschk::syschk_fatal(
                            lib::kernel_error::from_generic(
                                lib::generic_error::stall),
                            "watchdog: processor {} hard stall detected "
                            "(no heartbeat for {}s)",
                            cpu,
                            g_miss_count[cpu] * WATCHDOG_INTERVAL_SECONDS);
                    }
                    if (g_miss_count[cpu] >= WATCHDOG_MISS_THRESHOLD) {
                        palert("watchdog: processor {} stall suspected "
                              "(heartbeat={}, misses={}/{})",
                            cpu, hb, g_miss_count[cpu],
                            WATCHDOG_FATAL_THRESHOLD);
                    }
                } else {
                    g_miss_count[cpu] = 0;
                    g_last_heartbeat[cpu] = hb;
                }
            });

        if (g_harddog_armed) {
            harddog_ping();
        }
    }

} // namespace detail

namespace drivers::watchdog {

    auto disarm() noexcept -> void {
        using namespace arch::s390x::cpu;
        discard_value diag::diag288(diag::diag288_fn::cancel, diag::WDT_RESTART, 0, 0);
        detail::g_harddog_armed = false;
    }

    auto zi_watchdog_init(const zxfoundation::sys::zinit::zi_desc&) noexcept
        -> zxfoundation::sys::zinit::zi_result
    {
        using namespace detail;
        using namespace zxfoundation::sys::timer;
        using namespace zxfoundation::sys::printk;

        if (
            const condition_code rc =
                diag::diag288(diag::diag288_fn::init, diag::WDT_RESTART, diag::WDT_DEFAULT_TIMEOUT, 0);
            rc != 0
        ) {
            pnotice("watchdog: harddog not available (rc={})",
                     static_cast<i32>(rc));
        } else {
            g_harddog_armed = true;
            pinfo("watchdog: harddog armed (interval={}s)",
                  diag::WDT_DEFAULT_TIMEOUT);
        }

        auto& t = g_watchdog_timer;
        t.deadline_tod = clock::stck() + WATCHDOG_INTERVAL_TOD;
        t.interval_tod = WATCHDOG_INTERVAL_TOD;
        t.callback     = watchdog_check;
        t.data         = nullptr;
        t.mode         = timer_mode::periodic;
        discard_value timer_add(t);

        pinfo("watchdog: softdog armed (interval={}s, miss_threshold={})",
            WATCHDOG_INTERVAL_SECONDS, WATCHDOG_FATAL_THRESHOLD);
        return {};
    }
    __zinit__(late, p00, zi_watchdog_init, warn)

    auto heartbeat_bump() noexcept -> void {
        arch::s390x::cpu::percpu::this_cpu().watchdog_heartbeat++;
    }

} // namespace drivers::watchdog
