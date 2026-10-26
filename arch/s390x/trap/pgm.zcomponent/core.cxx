/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/trap/pgm.zcomponent/core.cxx
/// @brief Centralized pgm dispatch.

module arch.s390x.trap.pgm.core;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.psw;
import arch.s390x.trap.fixup.core;
import arch.s390x.cpu.teid;
import arch.s390x.cpu.features;
import arch.s390x.lib.stacktrace.core;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.percpu;
import zxfoundation.memory.hhdm;
import arch.s390x.cpu.pcss.core;
import arch.s390x.mmu;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.sys.irq.core;
import zxfoundation.base.types;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.timer.core;
import zxfoundation.sys.printk.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.domain.core;
import zxfoundation.memory.vm.fault;
import zxfoundation.memory.pmm;
import zxfoundation.dgp.domain.types;
import zxfoundation.base.typestate;
import zxfoundation.dgp.signal;
import zxfoundation.dgp.portal;
import lib.error;
import lib.stl;

from_global(nomangle) [[noreturn]] void sched_strand_trampoline_exit() noexcept;

namespace arch::s390x::trap::pgm::detail {
    
    /// @brief Log an unhandled domain program exception.
    no_export void log_domain_pgm(
        u16 pgm_code, const cpu::irq::irq_frame* frame,
        u64 trans_exc
    ) noexcept {
        zxfoundation::sys::printk::perr(
            "domain program check {:#06x} at PSW {:#018x}:{:#018x} "
            "tex={:#018x} domain={}\n",
            pgm_code, frame->psw_mask, frame->psw_addr, trans_exc,
            reinterpret_cast<zxfoundation::dgp::domain::domain*>(
                arch::s390x::cpu::lowcore::get_current()->get<"current_domain">()) ?
                reinterpret_cast<zxfoundation::dgp::domain::domain*>(
                    arch::s390x::cpu::lowcore::get_current()->get<"current_domain">())->ko.id :
                0xFFFFFFFFU);
    }

    template <typename T>
    struct pgm_name_entry {
        T                code{};
        std::string_view name{};
    };

    /// @brief Master program-interruption code names
    no_export constexpr pgm_name_entry<u16> master_pgm_names[] {
        { 0x0001, "operation" },                       
        { 0x0002, "privileged-operation" },             
        { 0x0003, "execute" },                          
        { 0x0004, "protection" },                       
        { 0x0005, "addressing" },                       
        { 0x0006, "specification" },                   
        { 0x0007, "data" },                             
        { 0x0008, "fixed-point-overflow" },            
        { 0x0009, "fixed-point-divide" },               
        { 0x000A, "decimal-overflow" },                 
        { 0x000B, "decimal-divide" },                   
        { 0x000C, "hfp-exponent-overflow" },            
        { 0x000D, "hfp-significance" },                 
        { 0x000E, "hfp-divide" },                       
        { 0x000F, "hfp-square-root" },                  
        { 0x0010, "segment-translation" },              
        { 0x0011, "page-translation" },                 
        { 0x0012, "translation-specification" },         
        { 0x0013, "special-operation" },                
        { 0x0015, "operand" },                         
        { 0x0016, "trace-table" },                     
        { 0x0017, "asn-translation-specification" },   
        { 0x0018, "transaction-constraint" },          
        { 0x001B, "vector-processing" },               
        { 0x001C, "space-switch-event" },              
        { 0x001D, "hfp-square-root" },                  
        { 0x001E, "crypto-operation" },                
        { 0x001F, "pc-translation-specification" },     
        { 0x0020, "afx-translation" },                 
        { 0x0021, "asx-translation" },                 
        { 0x0022, "lx-translation" },                 
        { 0x0023, "ex-translation" },                  
        { 0x0024, "primary-authority" },               
        { 0x0025, "secondary-authority" },            
        { 0x0026, "lfx-translation" },                 
        { 0x0027, "lsx-translation" },                 
        { 0x0028, "alet-specification" },              
        { 0x0029, "alen-translation" },                
        { 0x002A, "ale-sequence" },                    
        { 0x002B, "aste-validity" },                   
        { 0x002C, "aste-sequence" },                   
        { 0x002D, "extended-authority" },              
        { 0x002E, "lste-sequence" },                  
        { 0x002F, "aste-instance" },                  
        { 0x0030, "stack-full" },                      
        { 0x0031, "stack-empty" },                     
        { 0x0032, "stack-specification" },            
        { 0x0033, "stack-type" },                     
        { 0x0034, "stack-operation" },                
        { 0x0038, "asce-type" },                       
        { 0x0039, "region-first-translation" },        
        { 0x003A, "region-second-translation" },       
        { 0x003B, "region-third-translation" },        
        { 0x003D, "secure-storage-access" },           
        { 0x003E, "non-secure-storage-access" },       
        { 0x003F, "secure-storage-violation" },        
        { 0x0040, "monitor-event" },                   
        { 0x0080, "per-basic-event;per-nullification-event" }, 
        { 0x0119, "crypto-operation" },                 
        { 0x0200, "transactional-execution-aborted-event" }, 
        { 0x0218, "transactional-execution-constraint" },    
    };

    no_export constexpr pgm_name_entry<u8> dxc_pgm_names[] {
        { 0x00, "general-operand" },
        { 0x01, "afp-register" },
        { 0x02, "bfp-instruction" },
        { 0x03, "dfp-instruction" },
        { 0x04, "quantum-exception" },
        { 0x07, "simulated-quantum-exception" },
        { 0x08, "ieee-inexact-and-truncated" },
        { 0x0B, "simulated-ieee-inexact" },
        { 0x0C, "ieee-inexact-and-incremented" },
        { 0x10, "ieee-underflow-exact" },
        { 0x13, "simulated-ieee-underflow-exact" },
        { 0x18, "ieee-underflow-inexact-and-truncated" },
        { 0x1B, "simulated-ieee-underflow-inexact" },
        { 0x1C, "ieee-underflow-inexact-and-incremented" },
        { 0x20, "ieee-overflow-exact" },
        { 0x23, "simulated-ieee-overflow-exact" },
        { 0x28, "ieee-overflow-inexact-and-truncated" },
        { 0x2B, "simulated-ieee-overflow-inexact" },
        { 0x2C, "ieee-overflow-inexact-and-incremented" },
        { 0x40, "ieee-division-by-zero" },
        { 0x43, "simulated-ieee-division-by-zero" },
        { 0x80, "ieee-invalid-operation" },
        { 0x83, "simulated-ieee-invalid-operation" },
        { 0xFE, "vector-instruction" },
        { 0xFF, "compare-and-trap-instruction" },
    };

    no_export constexpr pgm_name_entry<u8> vxc_pgm_names[] {
        { 0b0001, "ieee-invalid-operation" },
        { 0b0010, "ieee-division-by-zero" },
        { 0b0011, "ieee-overflow" },
        { 0b0100, "ieee-underflow" },
        { 0b0101, "ieee-inexact" },
        { 0b1000, "integer-divide" },
    };

    /// @brief Determine whether the access at the given TEID was an
    ///        instruction-fetch.
    [[nodiscard]] inline auto is_exec_from_teid(cpu::teid::teid teid) noexcept -> bool {
        using cpu::features::facility;
        if (cpu::features::has(facility::instruction_exec_protection) &&
            cpu::teid::teid_prot_except_view{teid}.get<"!protection_cause">()
                == cpu::teid::teid_protection_cause::execution_protection)
            return true;
        if (cpu::features::has(facility::access_exc_fetch_store_ind))
            return cpu::teid::teid_prot_except_view{teid}.get<"access_exec_fs_id">()
                == cpu::teid::teid_aefsi::fetch_operation;
        return false;
    }

    /// @brief Determine whether the access at the given TEID was a store.
    [[nodiscard]] inline auto is_write_from_teid(cpu::teid::teid teid) noexcept -> bool {
        using cpu::features::facility;
        if (cpu::features::has(facility::access_exc_fetch_store_ind))
            return cpu::teid::teid_prot_except_view{teid}.get<"access_exec_fs_id">()
                == cpu::teid::teid_aefsi::store_operation;
        return true;
    }

    constexpr u16 PGM_PC_TRANSLATION_SPEC = 0x001FU;
    constexpr u16 PGM_AFX_TRANS  = 0x0020U;
    constexpr u16 PGM_ASTE_INSTANCE = 0x002FU;
    constexpr u16 PGM_STACK_FULL = 0x0030U;
    constexpr u16 PGM_STACK_SPECIFICATION = 0x0033U;
    constexpr u16 PGM_STACK_OPERATION = 0x0034U;

    [[nodiscard]] auto is_nullified_stack_check(const u16 code) noexcept -> bool {
        return code >= PGM_STACK_FULL && code <= PGM_STACK_OPERATION;
    }

    [[nodiscard]] auto is_pcpr_translation(const u16 code) noexcept -> bool {
        return code >= PGM_AFX_TRANS && code <= PGM_ASTE_INSTANCE;
    }

    [[nodiscard]] auto teid_class_name(const u64 teid_raw) noexcept -> std::string_view {
        u64 raw = teid_raw;
        switch (cpu::teid::teid_prot_except_view{raw}.get<"asce_identifier">()) {
            case cpu::teid::teid_asce_id::primary_asce:   return "primary";
            case cpu::teid::teid_asce_id::ar_mode:        return "ar";
            case cpu::teid::teid_asce_id::secondary_asce: return "secondary";
            case cpu::teid::teid_asce_id::home_asce:      return "home";
        }
        return "unknown";
    }

    [[nodiscard]] auto teid_access_name(const u64 teid_raw) noexcept -> std::string_view {
        if (is_exec_from_teid(teid_raw)) return "fetch";
        if (is_write_from_teid(teid_raw)) return "store";
        return "update";
    }

}

namespace arch::s390x::trap::pgm {

    [[nodiscard]] constexpr auto vxc_pgm_name_for(const vector_exception_code vxc) noexcept -> std::string_view {
        for (const auto& [c, name] : detail::vxc_pgm_names) {
            if (c == static_cast<u8>(vxc.get<"vic">())) return name;
        }
        return {};
    }

    [[nodiscard]] constexpr auto dxc_pgm_name_for(const u8 code) noexcept -> std::string_view {
        for (const auto& [c, name] : detail::dxc_pgm_names) {
            if (c == code) return name;
        }
        return {};
    }

    [[nodiscard]] constexpr auto master_pgm_name_for(const u16 code) noexcept -> std::string_view {
        for (const auto& [c, name] : detail::master_pgm_names) {
            if (c == code) return name;
        }
        return {};
    }

    /// @brief Centralized program-check dispatcher.
    auto handle(cpu::irq::irq_frame* frame) noexcept -> void {
        auto* lc = arch::s390x::cpu::lowcore::get_current();
        const u16 pgm_code     = lc->get<R"(pgm_interruption_id\instruction_code)">();
        u64 trans_exc    = lc->get<"translation_except_id">();
        const u32 data_exc_code = (pgm_code == PGM_DATA) ? static_cast<u32>(lc->get<R"(exception_code\data_exception_code)">()) : 0U;

        const bool in_problem_state =
            (frame->psw_mask & arch::s390x::cpu::psw::PSW_BIT_PSTATE) != 0;

        if (detail::is_nullified_stack_check(pgm_code)) {
            if (pgm_code == detail::PGM_STACK_FULL) {
                auto* strand = reinterpret_cast<zxfoundation::dgp::domain::strand*>(
                    arch::s390x::cpu::percpu::this_cpu().current_strand);
                if (strand != nullptr && strand->body != nullptr) {
                    auto& s = strand->body->linkage_stack;
                    u64 live_cr15 = 0;
                    arch::s390x::cpu::ctlreg::crread<15, 15>(live_cr15);
                    if (live_cr15 == 0U
                            && (s.cr15 != 0U || s.section_count != 0U)) {
                        // §5.12: impossible on a seeded stack — CR15 is
                        // restored from this very field at dispatch, so a
                        // zero register here means the cross-CPU CR15
                        // discipline regressed.  Fail loudly instead of
                        // growing a section nothing points at.
                        zxfoundation::sys::syschk::syschk_fatal(
                            ::lib::kernel_error::from_generic(
                                ::lib::generic_error::internal_error),
                            "pgm: stack-full with CR15=0 on seeded stack "
                            "strand={} stash={:#x} sections={}",
                            strand->ko.id, s.cr15, s.section_count);
                    }
                    // The register is the truth (it drifts forward with
                    // every push); adopt it before growing.
                    s.cr15 = live_cr15;
                    if (live_cr15 != 0U) {
                    }
                    auto grown = arch::s390x::cpu::pcss::stack_grow(s);
                    if (grown) {
                        if (live_cr15 == 0U) {
                            // Fresh stack: seed CR15 at the new header
                            // descriptor; nothing else points there.
                            arch::s390x::cpu::ctlreg::crwrite<15, 15>(s.cr15);
                        }
                        // Seeded case: CR15 stays at the old current
                        // entry — the re-executed instruction walks the
                        // chained trailer into the new section, writes
                        // its backward link, and advances CR15 itself.
                        return;
                    }
                    zxfoundation::sys::printk::perr(
                        "pgm: stack-full grow failed: {} strand={} "
                        "sections={} cr15={:#x}",
                        grown.error(), strand->ko.id, s.section_count,
                        live_cr15);
                }
            }
        }

        ::std::string_view pgm_name = master_pgm_name_for(pgm_code);

        if (!in_problem_state) {
            if (pgm_code == PGM_ADDRESSING || pgm_code == PGM_PROTECTION ||
                pgm_code == PGM_PAGE_TRANS    || pgm_code == PGM_SEGMENT_TRANS ||
                pgm_code == PGM_SPECIFICATION || pgm_code == PGM_OPERATION ||
                pgm_code == PGM_REGION_FIRST_TRANS  ||
                pgm_code == PGM_REGION_SECOND_TRANS ||
                pgm_code == PGM_REGION_THIRD_TRANS  ||
                pgm_code == PGM_TRANSLATION_SPEC) {
                if (fixup::try_applying_fixup(frame->psw_addr, frame)) {
                    return;
                }
            }

            // All other nucleus faults — kernel bug.
            lib::stacktrace::dump_stack_from_irq(frame);
            zxfoundation::sys::syschk::syschk_fatal(
                ::lib::kernel_error::from_generic(::lib::generic_error::internal_error),
                "pgm check {:#06x}{}{}{} at PSW {:#018x}:{:#018x} tex={:#018x}",
                pgm_code,
                pgm_name.empty() ? "" : " (", pgm_name, pgm_name.empty() ? "" : ")",
                static_cast<u64>(frame->psw_mask),
                static_cast<u64>(frame->psw_addr), trans_exc);
        }

        // §5.12: record mid-PC state for the interrupted strand.
        zxfoundation::dgp::domain::strand_note_interrupt_context(
            in_problem_state);

        auto* d = reinterpret_cast<zxfoundation::dgp::domain::domain*>(lc->get<"current_domain">());
        // §5.12: a user fault while the hardware is inside a stacking PC
        // belongs to the CALLEE domain — the PC switched PASN and
        // current_domain is the stale caller.  Resolve the real domain
        // by PASN; fatal faults then terminate the callee (and the
        // caller whose PC can no longer complete), never the wrong one.
        if (in_problem_state && d != nullptr) {
            u64 fault_pasn = 0;
            arch::s390x::cpu::ctlreg::crread<4, 4>(fault_pasn);
            const u16 asn = static_cast<u16>(fault_pasn & 0xFFFFULL);
            if (asn != 0U && static_cast<u16>(d->pc_asn) != asn) {
                zxfoundation::dgp::domain::domain* callee = nullptr;
                discard_value zxfoundation::dgp::domain::domain_for_each(
                    [&](zxfoundation::dgp::domain::domain& cand) noexcept -> bool {
                        if (static_cast<u16>(cand.pc_asn & 0xFFFFU) == asn) {
                            callee = &cand;
                            return false;
                        }
                        return true;
                    });
                if (callee != nullptr && callee != d) {
                    d = callee;
                }
            }
        }

        {
            u64 live_cr1 = 0;
            arch::s390x::cpu::ctlreg::crread<1, 1>(live_cr1);
            const bool borrowed_now = live_cr1 != arch::s390x::cpu::percpu::this_cpu().attached.asce;
            if (borrowed_now) {
            // Only stacked-state exception classes may consume a linkage
            // stack entry (PoP 5-12: PR/stack-operation family).  A plain
            // fault in a borrowed space must NOT unstack — the entry
            // belongs to an in-flight PC the fault interrupted.
            const bool stacked_state_class =
                pgm_code == detail::PGM_STACK_OPERATION ||
                pgm_code == detail::PGM_STACK_SPECIFICATION ||
                pgm_code == detail::PGM_PC_TRANSLATION_SPEC ||
                detail::is_pcpr_translation(pgm_code);
            auto* strand = reinterpret_cast<zxfoundation::dgp::domain::strand*>(
                arch::s390x::cpu::percpu::this_cpu().current_strand);
            if (stacked_state_class && strand != nullptr
                && strand->body != nullptr) {
                u64 cr15 = 0;
                arch::s390x::cpu::ctlreg::crread<15, 15>(cr15);
                const u64 entry_base = cr15 - 288U;
                if (arch::s390x::cpu::pcss::state_entry_restore(*frame, entry_base)) {
                    const auto* caller_dom =
                        static_cast<const zxfoundation::dgp::domain::domain*>(
                            strand->owner);
                    arch::s390x::cpu::pcss::state_entry_unstack(
                        strand->body->linkage_stack, entry_base,
                        caller_dom ? caller_dom->asce.val : 0ULL);
                    return;
                }
            }
            }
        }

        const bool is_translation_exception =
            pgm_code == PGM_SEGMENT_TRANS       ||
            pgm_code == PGM_PAGE_TRANS          ||
            pgm_code == PGM_TRANSLATION_SPEC    ||
            pgm_code == PGM_PROTECTION          ||
            pgm_code == PGM_REGION_FIRST_TRANS  ||
            pgm_code == PGM_REGION_SECOND_TRANS ||
            pgm_code == PGM_REGION_THIRD_TRANS  ||
            pgm_code == PGM_ASCE_TYPE           ||
            pgm_code == detail::PGM_PC_TRANSLATION_SPEC ||
            detail::is_pcpr_translation(pgm_code);

        {
            auto& pcp = arch::s390x::cpu::percpu::this_cpu();
            u64 cr1 = 0;
            u64 cr4 = 0;
            arch::s390x::cpu::ctlreg::crread<1, 1>(cr1);
            arch::s390x::cpu::ctlreg::crread<4, 4>(cr4);
            pcp.attached.publish_space(
                static_cast<u16>(cr4 & 0xFFFFULL),
                static_cast<u8>(cr1 != pcp.attached.asce ? 1U : 0U));
        }
        {
            constexpr u64 ps_dat = 0x0200'0000'0000'0000ULL;
            if (detail::is_pcpr_translation(pgm_code) &&
                ((frame->psw_mask & ps_dat) == 0ULL)) {
                goto portal_fault;
            }
        }

        if (is_translation_exception && d) {
            if (cpu::teid::teid_prot_except_view{trans_exc}.get<"asce_identifier">() == cpu::teid::teid_asce_id::ar_mode
                && (trans_exc & 0x00CULL) != 0ULL) {
                const u8 ar_num = lc->get<R"(exception_address_id\access_register_num)">();
                if (ar_num < 16U && frame->access_regs[ar_num] > 2U) {
                    // Portal-mediated access (§6.3): resolve against the
                    // live portal before any kill decision.  Revocation
                    // signals the accessor with the portal id packed in
                    // the payload (RC-06.4 — no unconditional kill);
                    // stale ALETs and out-of-contract accesses keep the
                    // legacy defaults.
                    const auto [outcome, portal_id] =
                        zxfoundation::dgp::portal::resolve_fault(
                            d->ko.id, ar_num,
                            frame->access_regs[ar_num],
                            trans_exc & ~0xFFFULL,
                            pgm_code == PGM_PROTECTION
                                ? detail::is_write_from_teid(trans_exc)
                                : false);
                    if (outcome == zxfoundation::dgp::portal::
                            fault_outcome::resolved) {
                        return;
                    }
                    if (outcome == zxfoundation::dgp::portal::
                            fault_outcome::revoked) {
                        zxfoundation::dgp::signal::signal_enqueue(
                            *d,
                            zxfoundation::dgp::signal::signal_kind::fault,
                            (static_cast<u64>(portal_id) << 44U)
                                | ((trans_exc & ~0xFFFULL) >> 12U));
                        zxfoundation::sys::printk::pnotice(
                            "pgm: portal fault revoked portal={} "
                            "va={:#x} — signaled, domain lives",
                            portal_id, trans_exc & ~0xFFFULL);
                        return;
                    }
                    goto portal_fault;
                }
            }
            const u64 fault_va = trans_exc & ~0xFFFULL;
            const bool is_write = (pgm_code == PGM_PROTECTION)
                ? detail::is_write_from_teid(trans_exc) : true;
            const bool is_exec  = detail::is_exec_from_teid(trans_exc);

            auto action = zxfoundation::memory::vm::handle_page_fault(
                d->vm_space, d->root_phys, d->storage_key,
                fault_va, is_write, is_exec);

            if (action == zxfoundation::memory::vm::fault_action::resolved ||
                action == zxfoundation::memory::vm::fault_action::stack_grew) {
                return;
            }
        }

portal_fault:
        if (d) {
            zxfoundation::dgp::signal::signal_enqueue(
                *d, zxfoundation::dgp::signal::signal_kind::fault,
                trans_exc & ~0xFFFULL);

            detail::log_domain_pgm(pgm_code, frame, trans_exc);
            if (detail::is_nullified_stack_check(pgm_code)) {
                {
                    u64 live_cr15 = 0;
                    arch::s390x::cpu::ctlreg::crread<15, 15>(live_cr15);
                    zxfoundation::sys::printk::perr(
                        "pgm: {} (nullified; no state change) at PSW {:#018x} cr15={:#x}\n",
                        pgm_name, static_cast<u64>(frame->psw_addr), live_cr15);
                }
            } else if (detail::is_pcpr_translation(pgm_code) ||
                       pgm_code == detail::PGM_PC_TRANSLATION_SPEC) {
                zxfoundation::sys::printk::perr(
                    "pgm: {} teid-class={} access={} at PSW {:#018x}\n",
                    pgm_name,
                    detail::teid_class_name(trans_exc),
                    detail::teid_access_name(trans_exc),
                    static_cast<u64>(frame->psw_addr));
            }
            if (pgm_code == PGM_DATA) {
                const auto dxc_name = dxc_pgm_name_for(
                    static_cast<u8>(data_exc_code & 0xFFU));
                zxfoundation::sys::printk::perr(
                    "pgm: data-exception dxc={:#04x}{}\n",
                    static_cast<u8>(data_exc_code & 0xFFU),
                    dxc_name);
            }

            discard_value zxfoundation::scoms::kobject::mark_dying(d);
            d->flags = d->flags | zxfoundation::dgp::domain::domain_flags::need_resched;

            // §5.12: fatal fault mid-PC — the interrupted strand belongs
            // to the CALLER (its PC can never complete now); terminate it
            // too, then land the interrupted strand on the sched exit.
            auto* interrupted = reinterpret_cast<zxfoundation::dgp::domain::strand*>(
                arch::s390x::cpu::percpu::this_cpu().current_strand);
            if (interrupted != nullptr && interrupted->body != nullptr
                && interrupted->body->in_pc != 0U && interrupted->owner != nullptr
                && interrupted->owner != d) {
                auto* caller = static_cast<zxfoundation::dgp::domain::domain*>(
                    interrupted->owner);
                zxfoundation::sys::printk::perr(
                    "pgm: fatal callee fault mid-PC — terminating caller "
                    "domain {} too",
                    caller->ko.id);
                discard_value zxfoundation::scoms::kobject::mark_dying(caller);
                caller->flags = caller->flags
                    | zxfoundation::dgp::domain::domain_flags::need_resched;
            }

            frame->gprs[15] = d->primary_strand.kstack_top - 160U;
            frame->psw_mask = arch::s390x::cpu::psw::PSW_MASK_KERNEL_DAT;
            frame->psw_addr = reinterpret_cast<u64>(&sched_strand_trampoline_exit);
        } else {
            zxfoundation::sys::syschk::syschk_fatal(
                ::lib::kernel_error::from_generic(::lib::generic_error::internal_error),
                "domain pgm check {:#06x}{}{}{} at PSW {:#018x}:{:#018x} "
                "(no current domain)",
                pgm_code,
                pgm_name.empty() ? "" : " (", pgm_name, pgm_name.empty() ? "" : ")",
                static_cast<u64>(frame->psw_mask),
                static_cast<u64>(frame->psw_addr));
        }
    }

} // namespace arch::s390x::trap::pgm
