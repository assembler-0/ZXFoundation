/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/trap/mcck.zcomponent/core.cxx
/// @brief Centralized Machine-Check (MCCK) dispatcher.

module arch.s390x.trap.mcck.core;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.clock;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.features;
import zxfoundation.sys.irq.core;
import zxfoundation.sys.irq.consts;
import zxfoundation.base.types;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.printk.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.signal;
import zxfoundation.dgp.domain.types;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.sched.core;
import lib.error;
import lib.bitfield;

namespace arch::s390x::trap::mcck::detail {

    /// @brief Validate register-state validit
    [[nodiscard]] auto nmi_registers_valid(u64 mcic) noexcept -> bool {
        constexpr u64 required =
            MCIC_GR | MCIC_FP | MCIC_FC | MCIC_AR |
            MCIC_MS | MCIC_PM | MCIC_IA;
        if ((mcic & required) != required) return false;
        if ((mcic & MCIC_VR) == 0ULL) return false;
        return true;
    }

    /// @brief Structured MCESA diagnosis dump.
    auto dump_mcesa(const arch::s390x::cpu::lowcore::zlowcore& lc) noexcept
        -> void
    {
        zxfoundation::sys::printk::pcrit(
            "mcck: mcesa dump — psw={:#018x}:{:#018x}\n",
            static_cast<u64>(lc.get<R"(psw_save_area\mask)">().underlying()),
            static_cast<u64>(lc.get<R"(psw_save_area\address)">()));
        zxfoundation::sys::printk::pcrit(
            "mcck: mcesa gprs:\n  {:#018x} {:#018x} {:#018x} {:#018x}\n  "
            "{:#018x} {:#018x} {:#018x} {:#018x}\n  "
            "{:#018x} {:#018x} {:#018x} {:#018x}\n  "
            "{:#018x} {:#018x} {:#018x} {:#018x}\n",
            lc.get<"gpregs_save_area">()[0],  lc.get<"gpregs_save_area">()[1],
            lc.get<"gpregs_save_area">()[2],  lc.get<"gpregs_save_area">()[3],
            lc.get<"gpregs_save_area">()[4],  lc.get<"gpregs_save_area">()[5],
            lc.get<"gpregs_save_area">()[6],  lc.get<"gpregs_save_area">()[7],
            lc.get<"gpregs_save_area">()[8],  lc.get<"gpregs_save_area">()[9],
            lc.get<"gpregs_save_area">()[10], lc.get<"gpregs_save_area">()[11],
            lc.get<"gpregs_save_area">()[12], lc.get<"gpregs_save_area">()[13],
            lc.get<"gpregs_save_area">()[14], lc.get<"gpregs_save_area">()[15]);
        zxfoundation::sys::printk::pcrit(
            "mcck: mcesa CRS:\n  {:#018x} {:#018x} {:#018x} {:#018x}\n",
            lc.get<"cregs_save_area">()[0],  lc.get<"cregs_save_area">()[1],
            lc.get<"cregs_save_area">()[2],  lc.get<"cregs_save_area">()[3]);
        zxfoundation::sys::printk::pcrit(
            "mcck: mcesa — last_break={:#018x} prefix={:#010x} "
            "fpc={:#010x} tod_pg={:#010x}\n",
            lc.get<"last_break_save_area">(), lc.get<"prefixreg_save_area">(),
            lc.get<"fpt_creg_save_area">(), lc.get<"tod_progreg_save_area">());
        zxfoundation::sys::printk::pcrit(
            "mcck: mcesa — cpu_timer={:#010x}{:#010x} clock_comp="
            "{:#010x}{:#010x}\n",
            lc.get<"cpu_timer_save_area">()[1], lc.get<"cpu_timer_save_area">()[0],
            lc.get<"clock_comp_save_area">()[1], lc.get<"clock_comp_save_area">()[0]);
    }

    [[nodiscard]] auto ipd_window_expired(
        arch::s390x::cpu::percpu::percpu_data& pcp,
        u64 now
    ) noexcept -> bool {
        return now >= pcp.ipd_window_deadline;
    }

} // namespace detail

namespace arch::s390x::trap::mcck {

    /// @brief Centralized machine-check dispatcher
    /// @param[in] frame  Saved interrupt frame from entry.S SAVE_FRAME on the
    ///                   MCCK stack.
    auto handle(cpu::irq::irq_frame* frame) noexcept -> void {
        auto* lc = cpu::lowcore::get_current();
        const u64 mcic = lc->get<"mcck_interruption_code">();
        if (mcic == 0ULL) return;

        lc->set<"mcck_interruption_code">(0ULL);

        if ((mcic & MCIC_BACKLOG) != 0ULL) {
            zxfoundation::sys::syschk::syschk_warning(
                ::lib::kernel_error::from_machine(
                    ::lib::machine_error::backlog),
                "mcck: backlog set — stacked machine-check detected; "
                "prior MCCK not fully processed before this interruption");
        }

        if ((mcic & MCIC_SD) != 0ULL) {
            if ((mcic & MCIC_AP) != 0ULL)
                zxfoundation::sys::printk::pnotice(
                    "mcck: ancillary-report (AP) available\n");
            zxfoundation::sys::syschk::syschk_fatal(
                ::lib::kernel_error::from_machine(::lib::machine_error::system_damage),
                "mcck: system damage — unrecoverable");
        }

        if ((mcic & MCIC_CD) != 0ULL) {
            zxfoundation::sys::syschk::syschk_fatal(
                ::lib::kernel_error::from_machine(::lib::machine_error::timing_facility),
                "mcck: timing-facility damage — TOD / clock comparator stale");
        }

        if ((mcic & MCIC_PD) != 0ULL) {
            if ((mcic & MCIC_B) != 0ULL) {
                auto& pcp = cpu::percpu::this_cpu();
                const u64 now = static_cast<u64>(cpu::clock::stcke().get<"tod">());
                if (detail::ipd_window_expired(pcp, now)) {
                    pcp.ipd_count = 0U;
                    pcp.ipd_window_deadline = now + MAX_IPD_WINDOW_TOD;
                }
                ++pcp.ipd_count;
                if (pcp.ipd_count >= MAX_IPD_COUNT) {
                    zxfoundation::sys::syschk::syschk_critical(
                        ::lib::kernel_error::from_machine(
                            ::lib::machine_error::inst_process_damage),
                        "mcck: {} nullifying IPDs within 5 min — unrecoverable",
                        pcp.ipd_count);
                } else {
                    zxfoundation::sys::syschk::syschk_notice(
                        ::lib::kernel_error::from_machine(
                            ::lib::machine_error::inst_process_damage),
                        "mcck: nullifying IPD ({} of {}) — survived",
                        pcp.ipd_count, MAX_IPD_COUNT);
                }
            } else {
                // PD without B: straight fatal — instruction was not nullified.
                zxfoundation::sys::syschk::syschk_critical(
                    ::lib::kernel_error::from_machine(
                        ::lib::machine_error::inst_process_damage),
                    "mcck: instruction-processing damage — unrecoverable");
            }
        }

        if ((mcic & MCIC_ED) != 0ULL && (mcic & MCIC_EC) != 0ULL) {
            const u32 ext_code = lc->get<"external_damage_code">().underlying();
            zxfoundation::sys::syschk::syschk_warning(
                ::lib::kernel_error::from_machine(::lib::machine_error::external_damage),
                "mcck: external damage code={:#010x}", ext_code);
            zxfoundation::sys::irq::deferred_set(
                zxfoundation::sys::irq::DEFERRED_MCCK_PENDING_BIT);
        }

        if ((mcic & MCIC_SE) != 0ULL) {
            zxfoundation::sys::syschk::syschk_fatal(
                ::lib::kernel_error::from_machine(
                    ::lib::machine_error::storage_error_uncorrected),
                "mcck: storage error uncorrected");
        }
        if ((mcic & MCIC_KE) != 0ULL) {
            zxfoundation::sys::syschk::syschk_fatal(
                ::lib::kernel_error::from_machine(
                    ::lib::machine_error::storage_key_error),
                "mcck: storage-key error uncorrected");
        }
        if ((mcic & MCIC_DS) != 0ULL && (mcic & MCIC_FA) != 0ULL) {
            const u64 fsa = lc->get<"failing_storage_address">();
            zxfoundation::sys::syschk::syschk_fatal(
                ::lib::kernel_error::from_machine(
                    ::lib::machine_error::storage_degradation),
                "mcck: storage degradation at fsa={:#018x}", fsa);
        } else if ((mcic & MCIC_DS) != 0ULL) {
            zxfoundation::sys::syschk::syschk_fatal(
                ::lib::kernel_error::from_machine(
                    ::lib::machine_error::storage_degradation),
                "mcck: storage degradation — failing address unknown");
        }
        if ((mcic & MCIC_FA) != 0ULL) {
            const u64 fsa = lc->get<"failing_storage_address">();
            zxfoundation::sys::syschk::syschk_warning(
                ::lib::kernel_error::from_machine(
                    ::lib::machine_error::failing_storage_addr),
                "mcck: failing storage address={:#018x}", fsa);
            zxfoundation::sys::irq::deferred_set(
                zxfoundation::sys::irq::DEFERRED_MCCK_PENDING_BIT);
        }

        if ((mcic & MCIC_SR) != 0ULL) {
            zxfoundation::sys::syschk::syschk_notice(
                ::lib::kernel_error::from_machine(::lib::machine_error::system_recovery),
                "mcck: system recovery completed");
        }
        if ((mcic & MCIC_DG) != 0ULL) {
            zxfoundation::sys::syschk::syschk_warning(
                ::lib::kernel_error::from_machine(::lib::machine_error::degradation),
                "mcck: degradation reported");
            zxfoundation::sys::irq::deferred_set(
                zxfoundation::sys::irq::DEFERRED_MCCK_PENDING_BIT);
        }
        if ((mcic & MCIC_W) != 0ULL) {
            zxfoundation::sys::syschk::syschk_notice(
                ::lib::kernel_error::from_machine(::lib::machine_error::warning),
                "mcck: warning pending");
            zxfoundation::sys::irq::deferred_set(
                zxfoundation::sys::irq::DEFERRED_MCCK_PENDING_BIT);
        }
        if ((mcic & MCIC_CP) != 0ULL) {
            zxfoundation::sys::syschk::syschk_notice(
                ::lib::kernel_error::from_machine(::lib::machine_error::channel_report_pending),
                "mcck: channel-report pending — CRW handling armed");
            zxfoundation::sys::irq::deferred_set(
                zxfoundation::sys::irq::DEFERRED_MCCK_PENDING_BIT);
        }
        if ((mcic & MCIC_SP) != 0ULL) {
            zxfoundation::sys::syschk::syschk_warning(
                ::lib::kernel_error::from_machine(
                    ::lib::machine_error::service_processor_damage),
                "mcck: service-processor damage");
        }
        if ((mcic & MCIC_CK) != 0ULL) {
            zxfoundation::sys::syschk::syschk_warning(
                ::lib::kernel_error::from_machine(
                    ::lib::machine_error::channel_subsystem_damage),
                "mcck: channel-subsystem damage");
        }
        if ((mcic & MCIC_SC) != 0ULL) {
            zxfoundation::sys::printk::pinfo(
                "mcck: storage error corrected (informational)\n");
        }

        if (!detail::nmi_registers_valid(mcic)) {
            const bool in_user =
                (frame->psw_mask & cpu::psw::PSW_BIT_PSTATE) != 0;
            if (!in_user) {
                detail::dump_mcesa(*lc);
                zxfoundation::sys::syschk::syschk_fatal(
                    ::lib::kernel_error::from_machine(::lib::machine_error::system_damage),
                    "mcck: nmi registers invalid in nucleus — register state unreliable");
            }
            auto* d =
                reinterpret_cast<zxfoundation::dgp::domain::domain*>(lc->get<"current_domain">());
            if (d != nullptr) {
                zxfoundation::dgp::signal::signal_enqueue(
                    *d,
                    zxfoundation::dgp::signal::signal_kind::fault,
                    static_cast<u64>(lc->get<"failing_storage_address">()));
                zxfoundation::sched::sched_resched_current();
            }
            detail::dump_mcesa(*lc);
        }

        zxfoundation::sys::irq::irq_dispatch(
            zxfoundation::sys::irq::IRQ_BASE_MCCK, frame,
            arch::s390x::cpu::irq::ZX_IRQ_CLASS_MCCK);
    }

} // namespace arch::s390x::trap::mcck
