/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/printk.cxxm
/// @brief Kernel logging framework.

module zxfoundation.sys.printk.format;
import zxfoundation.sys.printk.types;
import zxfoundation.base.types;
import zxfoundation.sys.time.core;
import zxfoundation.domain.domain_types;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.lowcore;
import lib.format;
import std;

namespace zxfoundation::sys::printk {

    auto prepare_header(log_level level, char buff[], usize size) noexcept -> usize {
        usize offset{0};
        const auto now = time::monotonic_now();
        const u64 s = now.nanoseconds_since_epoch() / 1000000000U;
        const u64 us = (now.nanoseconds_since_epoch() % 1000000000U) / 1000U;
        const u16 cpu = arch::s390x::cpu::percpu::cpu_id();
        const auto prefix = arch::s390x::cpu::percpu::cpu_type_prefix();

        char tag{};
        switch (level) {
            case log_level::EMERG:  tag = 'X'; break;
            case log_level::ALERT:  tag = 'A'; break;
            case log_level::CRIT:   tag = 'C'; break;
            case log_level::ERROR:  tag = 'E'; break;
            case log_level::WARN:   tag = 'W'; break;
            case log_level::NOTICE: tag = 'N'; break;
            case log_level::INFO:   tag = 'I'; break;
            case log_level::VERBOSE:tag = 'V'; break;
            default:                tag = '?'; break;
        }

        // Format: ZXF!-[(SSSS).uuuuuu] [(type)NN] T@<logged_domain> <message>\n
        std::string_view logged_domain_name = "nucleus";
        {
            if (const auto* dom_ptr = arch::s390x::cpu::lowcore::current_domain_ptr();
                dom_ptr != nullptr) [[likely]] {
                if (auto* dom = static_cast<const zxfoundation::domain::domain*>(dom_ptr);
                    dom->name != nullptr && dom->name[0] != '\0') {
                    logged_domain_name = dom->name;
                }
            }
        }

        if (s > 1'000'000'000U) [[unlikely]] {
            offset = lib::format::format_to({buff, size},
                "ZXF!-[(XXXX).xxxxxx] [({}){}] ({})@[{}] ",
                prefix,
                lib::format::dec{static_cast<u64>(cpu), 3, true},
                tag,
                logged_domain_name
            );
        } else {
            offset = lib::format::format_to({buff, size},
                "ZXF!-[({}).{}] [({}){}] ({})@[{}] ",
                lib::format::dec{s, 4, true},
                lib::format::dec{us, 6, true},
                prefix,
                lib::format::dec{static_cast<u64>(cpu), 3, true},
                tag,
                logged_domain_name
            );
        }
        return offset;
    }

} // namespace zxfoundation::sys::printk
