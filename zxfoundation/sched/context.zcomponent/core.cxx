/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/context.zcomponent/core.cxx
/// @brief Execution context classification for z/Architecture.

module zxfoundation.sched.context.core;
import zxfoundation.sys.syschk;
import lib.error;
import lib.stl;

namespace zxfoundation::sched::context {

    // Assertion primitives -- panic via disabled-wait PSW
    auto assert_irqs_disabled(
        std::source_location loc
    ) noexcept -> void {
        if (!irqs_disabled()) [[unlikely]] {
           sys::syschk::system_check(
               lib::kernel_error::from_generic(lib::generic_error::internal_error),
               sys::syschk::syschk_severity::fatal,
               "sched: irq assertion failure",
               loc
            );
        }
    }

    auto assert_not_in_interrupt(
        std::source_location loc
    ) noexcept -> void {
        if (in_interrupt()) [[unlikely]] {
            sys::syschk::system_check(
              lib::kernel_error::from_generic(lib::generic_error::internal_error),
              sys::syschk::syschk_severity::fatal,
              "sched: interrupt context assertion failure",
              loc
           );;
        }
    }

    auto assert_not_in_hardirq(
        std::source_location loc
    ) noexcept -> void {
        if (in_hardirq()) [[unlikely]] {
            sys::syschk::system_check(
              lib::kernel_error::from_generic(lib::generic_error::internal_error),
              sys::syschk::syschk_severity::fatal,
              "sched: hardirq context assertion failure",
              loc
           );
        }
    }

    auto assert_can_sleep(
        std::source_location loc
    ) noexcept -> void {
        if (!can_sleep()) [[unlikely]] {
            sys::syschk::system_check(
              lib::kernel_error::from_generic(lib::generic_error::internal_error),
              sys::syschk::syschk_severity::fatal,
              "sched: sleep context assertion failure",
              loc
           );
        }
    }

} // namespace zxfoundation::sched::context
