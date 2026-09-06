/// SPDX-License-Identifier: Apache-2.0
/// @file io.cxxm
/// @brief Centralized io dispatch.

module arch.s390x.trap.io.core;
import arch.s390x.cpu.lowcore;
import arch.s390x.io.css.subchannel;
import arch.s390x.cpu.irq;
import zxfoundation.sys.irq.core;
import zxfoundation.sys.irq.consts;
import zxfoundation.base.types;

namespace arch::s390x::trap::io {

    /// @brief Centralized io dispatcher.
    auto handle(cpu::irq::irq_frame* frame) noexcept -> void {
        const auto* lc = cpu::lowcore::get_current();
        const u16 sch_no       = lc->get<R"(io_interruption_code\sid\sch_no)">();
        const u32 io_int_parm  = lc->get<R"(io_interruption_code\interruption_parameter)">();

        // Dispatch to the CSS layer for command-RB / TSCH processing.
        s390x::io::css::io_interrupt(sch_no);

        const u8 isc = static_cast<u8>(io_int_parm & 0x07U);
        const u16 io_irq = static_cast<u16>(
            zxfoundation::sys::irq::IRQ_BASE_IO + isc);
        zxfoundation::sys::irq::irq_dispatch(io_irq, frame, cpu::irq::ZX_IRQ_CLASS_IO);
    }

} // namespace arch::s390x::trap::io
