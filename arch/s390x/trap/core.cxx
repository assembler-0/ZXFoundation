/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/trap/trap.cxxm
/// @brief interrupt handlers

import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.irq;
import arch.s390x.trap.io;
import arch.s390x.trap.svc;
import arch.s390x.trap.mcck;
import arch.s390x.trap.ext;
import arch.s390x.trap.pgm;
import zxfoundation.base.types;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.irq.core;
import zxfoundation.sys.irq.consts;
import lib.error;
import lib.softirq.core;

/// @brief Program check handler.
declscope(C) auto do_pgm_interrupt(arch::s390x::cpu::irq::irq_frame* frame) noexcept -> void {
    arch::s390x::trap::pgm::handle(frame);
    namespace irq = zxfoundation::sys::irq;
    if (irq::deferred_test_clear(irq::DEFERRED_IRQ_WORK_BIT)) {
        lib::do_softirq();
    }
}

/// @brief External interrupt handler.
declscope(C) auto do_ext_interrupt(arch::s390x::cpu::irq::irq_frame* frame) noexcept -> void {
    arch::s390x::trap::ext::handle(frame);
}

/// @brief I/O interrupt handler.
declscope(C) auto do_io_interrupt(arch::s390x::cpu::irq::irq_frame* frame) noexcept -> void {
    arch::s390x::trap::io::handle(frame);
    namespace irq = zxfoundation::sys::irq;
    if (irq::deferred_test_clear(irq::DEFERRED_IRQ_WORK_BIT)) {
        lib::do_softirq();
    }
}

/// @brief SVC handler — nucleus-call dispatch.
/// @note Drains deferred softirq work after the nucleus call completes.
declscope(C) auto do_svc_interrupt(arch::s390x::cpu::irq::irq_frame* frame, u16 svc_code) noexcept -> void {
    frame->gprs[2] = arch::s390x::trap::svc::handle(frame, svc_code);
    namespace irq = zxfoundation::sys::irq;
    if (irq::deferred_test_clear(irq::DEFERRED_IRQ_WORK_BIT)) {
        lib::do_softirq();
    }
}

/// @brief Machine-check handler — decode MCIC and attempt recovery.
declscope(C) auto do_mcck_interrupt(arch::s390x::cpu::irq::irq_frame* frame) noexcept -> void {
    arch::s390x::trap::mcck::handle(frame);
}

/// @brief Restart interruption handler.
declscope(C) [[noreturn]] auto do_restart_interrupt(
    void* data,
    i32   source,
    u16   cpu_id,
    u32   flags
) noexcept -> void {
    discard_value source;
    discard_value flags;
    discard_value cpu_id;
    auto& lc = *arch::s390x::cpu::lowcore::get_current();
    const auto fn = reinterpret_cast<void(*)(void*)>(lc.restart_fn);
    if (!fn) {
        zxfoundation::sys::syschk::syschk_fatal(
            ::lib::kernel_error::from_generic(::lib::generic_error::internal_error),
            "restart: sigp restart on cpu {} without registered handler",
            cpu_id);
    }
    fn(data);
    zxfoundation::sys::syschk::syschk_critical(
        ::lib::kernel_error::from_generic(::lib::generic_error::internal_error),
        "restart: handler returned on cpu {}", cpu_id);
}