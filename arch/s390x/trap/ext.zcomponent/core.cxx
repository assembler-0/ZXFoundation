/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/trap/ext.zcomponent/core.cxx
/// @brief Centralized external-interruption dispatcher.

module arch.s390x.trap.ext.core;
import arch.s390x.cpu.percpu;
import zxfoundation.dgp.domain.core;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.cputimer;
import arch.s390x.cpu.clock;
import arch.s390x.cpu.cpumask;
import arch.s390x.cpu.smp.core;
import arch.s390x.cpu.psw;
import zxfoundation.sys.irq.consts;
import zxfoundation.sys.irq.core;
import zxfoundation.sched.core;
import zxfoundation.sched.topology.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.base.types;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.timer.core;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.timer.types;
import zxfoundation.smp.csd.core;
import lib.error;
import lib.softirq.core;

namespace arch::s390x::trap::ext::detail {

    /// @brief Acknowledge the current external interrupt in lowcore.
    auto ack_lowcore() noexcept -> void {
        cpu::lowcore::get_current()->set<"external_interruption_code">(0U);
    }

} // namespace detail

namespace arch::s390x::trap::ext {

    /// @brief Centralized external-interruption dispatcher.
    /// @param[in,out] frame  Saved interrupt frame from entry.S.
    auto handle(cpu::irq::irq_frame* frame) noexcept -> void {
        // §5.12: record whether the hardware was inside a stacking PC
        // (PASN switched to a callee) when this interruption arrived.
        zxfoundation::dgp::domain::strand_note_interrupt_context(
            (frame->psw_mask & arch::s390x::cpu::psw::PSW_BIT_PSTATE) != 0);
        auto* lc = cpu::lowcore::get_current();
        const u16 ext_code = lc->get<"external_interruption_code">();

        detail::ack_lowcore();

        const u16 irq_nr = zxfoundation::sys::irq::ext_code_to_irq(ext_code);
        if (irq_nr == zxfoundation::sys::irq::IRQ_NR_MAX) {
            zxfoundation::sys::printk::pverbose(
                "ext: unrecognised ext_code {:#06x} — no IRQ slot\n",
                ext_code);
            return;
        }

        zxfoundation::sys::irq::irq_dispatch(
            irq_nr, frame, cpu::irq::ZX_IRQ_CLASS_EXT);

        namespace irq = zxfoundation::sys::irq;
        if (irq::deferred_test_clear(irq::DEFERRED_RESCHED_BIT)) {
            zxfoundation::sched::sched_resched_current();
        }

        if (irq::deferred_test_clear(irq::DEFERRED_MCCK_PENDING_BIT)) {
            // The deferred-MCCK handler stays inline in the MCCK
            // dispatcher; the ch. 10.2 handler lands at R4 (OBL-015,
            // BUG-045). The bit is acknowledged here so the trap path
            // stays self-consistent.
            zxfoundation::sys::printk::pverbose(
                "ext: deferred MCCK work armed on cpu={} (TODO!)\n",
                arch::s390x::cpu::percpu::this_cpu().cpu_id);
        }

        if (irq::deferred_test_clear(irq::DEFERRED_CALL_FUNCTION_BIT)) {
            zxfoundation::smp::csd::drain_cpu();
        }

        if (irq::deferred_test_clear(irq::DEFERRED_IRQ_WORK_BIT)) {
            lib::do_softirq();
        }

        if (irq::deferred_test_clear(irq::DEFERRED_STOP_CPU_BIT)) {
            const auto cpu_id =
                arch::s390x::cpu::percpu::this_cpu().cpu_id;
            zxfoundation::sys::printk::pnotice(
                "ext: stop-cpu deferred work on cpu={} — halting\n", cpu_id);
            arch::s390x::cpu::smp::request_stop_current();
            arch::s390x::cpu::psw::disabled_wait();
        }
    }

} // namespace arch::s390x::trap::ext
