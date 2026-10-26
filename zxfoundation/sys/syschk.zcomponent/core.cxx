/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/syschk.zcomponent/core.cxx
/// @brief Last-resort global system teardown (system check).

module zxfoundation.sys.syschk.core;
import zxfoundation.sys.syschk.types;
import zxfoundation.sys.printk.core;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sys.printk.types;
import zxfoundation.sys.zinit.core;
import arch.s390x.lib.stacktrace.core;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.ipi;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.lowcore;
import arch.s390x.init.zxfl.protocol;
import lib.error;
import lib.cstring;
import lib.format;
import lib.stl;
import drivers.watchdog.core;

#define __zxmacro_want_zinit
#include macro_extensions


namespace zxfoundation::sys::syschk::detail {

    no_export constexpr auto ZXF_BUG_REPORT_URL_HTTPS = "https://github.com/assembler-0/ZXFoundation/issues";

    /// @brief Atomic flag: set to 1 by the first CPU to call system_check.
    no_export std::atomic<u32> g_halting{0};

    no_export auto to_log_level(syschk_severity sev) -> printk::log_level {
        switch (sev) {
            case syschk_severity::notice: return zxfoundation::sys::printk::log_level::NOTICE;
            case syschk_severity::warning: return zxfoundation::sys::printk::log_level::WARN;
            case syschk_severity::critical: return zxfoundation::sys::printk::log_level::CRIT;
            case syschk_severity::fatal:
            default: return zxfoundation::sys::printk::log_level::EMERG;
        }
    }

} // namespace detail

namespace zxfoundation::sys::syschk {

    /// @brief Record a kernel error. Conditionally halt.
    /// @param[in] err      The @c lib::kernel_error that triggered this check.
    ///                     Its @c category and @c code are stored verbatim in the
    ///                     crash record — no translation occurs.
    /// @param[in] sev      Severity annotation.  Affects the log level used and the
    ///                     @c severity byte of the crash record.  All severity levels
    ///                     currently result in an unconditional halt.
    /// @param[in] msg      Human-readable reason string.  Truncated to
    ///                     @c ZX_CRASH_MSG_LEN - 1 bytes.
    /// @param[in] location Source location of the call site.
    [[gnu::noinline]] auto system_check(
        lib::kernel_error    err,
        syschk_severity      sev,
        std::string_view     msg,
        std::source_location location
    ) noexcept -> void {

        discard_value arch::s390x::cpu::irq::irq_save_disable();

        u32 expected = 0;
        if (!detail::g_halting.compare_exchange_strong(
                expected, 1U,
                std::memory_order::seq_cst,
                std::memory_order::relaxed)) [[unlikely]] {
            drivers::watchdog::disarm();
            arch::s390x::cpu::psw::disabled_wait();
        }

        auto* rec = reinterpret_cast<zx_crash_record*>(
            static_cast<uptr>(ZX_CRASH_RECORD_OFFSET));

        rec->magic        = ZX_CRASH_RECORD_MAGIC;
        rec->severity     = static_cast<u8>(sev);
        rec->err_category = static_cast<u8>(err.category);
        rec->err_code     = err.code;
        rec->cpu_id       = arch::s390x::cpu::percpu::cpu_id();

        const auto current_psw = arch::s390x::cpu::psw::extract_psw();
        rec->psw_mask = current_psw.get<"mask">().underlying();
        rec->psw_addr = current_psw.get<"address">();

        lib::string_copy(rec->msg, msg);

        printk::printk(detail::to_log_level(sev),
            "syschk: [class={} category={}]\n",
            severity_name(sev),
            err
        );

        printk::printk(detail::to_log_level(sev), "syschk: from {}\n", location);
        printk::printk(detail::to_log_level(sev), "syschk: {}\n",msg);
        arch::s390x::lib::stacktrace::dump_stack();

        if (sev < syschk_severity::critical) return;
        printk::printk(detail::to_log_level(sev), "syschk: abnormal end state. nucleus will not continue.");
        printk::printk(detail::to_log_level(sev),
            "syschk: if you believe this is a nucleus fault, please report a bug to <{}>",
            detail::ZXF_BUG_REPORT_URL_HTTPS
        );

        const u16 self_cpu_id = arch::s390x::cpu::percpu::cpu_id();
        arch::s390x::cpu::cpumask::for_each_online_cpu(
            [self_cpu_id](u16 cpu) noexcept -> void {
                if (cpu == self_cpu_id) [[unlikely]] return;
                arch::s390x::cpu::ipi::send_halt_ipi(cpu);
            });

        drivers::watchdog::disarm();
        arch::s390x::cpu::psw::disabled_wait();
        __builtin_unreachable();
    }

    /// @brief initialize the system check infrastructure
    auto zi_syschk_init(const zinit::zi_desc&) noexcept -> zinit::zi_result {
        return {};
    }
    __zinit__(early, p40, zi_syschk_init, fatal);

} // namespace zxfoundation::sys::syschk
