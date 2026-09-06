/// SPDX-License-Identifier: Apache-2.0
/// @file pgm.cxxm
/// @brief Centralized pgm dispatch.

module arch.s390x.trap.pgm.core;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.psw;
import arch.s390x.trap.fixup.core;
import arch.s390x.cpu.teid;
import arch.s390x.cpu.features;
import arch.s390x.lib.stacktrace.core;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.scoms.kobject.types;
import zxfoundation.sys.irq.core;
import zxfoundation.base.types;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.timer.core;
import zxfoundation.sys.printk.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.memory.vm.fault;
import zxfoundation.dgp.domain.types;
import zxfoundation.base.typestate;
import zxfoundation.dgp.signal;
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

        auto* d = reinterpret_cast<zxfoundation::dgp::domain::domain*>(lc->get<"current_domain">());

        const bool is_translation_exception =
            pgm_code == PGM_SEGMENT_TRANS       ||
            pgm_code == PGM_PAGE_TRANS          ||
            pgm_code == PGM_TRANSLATION_SPEC    ||
            pgm_code == PGM_PROTECTION          ||
            pgm_code == PGM_REGION_FIRST_TRANS  ||
            pgm_code == PGM_REGION_SECOND_TRANS ||
            pgm_code == PGM_REGION_THIRD_TRANS  ||
            pgm_code == PGM_ASCE_TYPE;

        if (is_translation_exception && d) {
            if (cpu::teid::teid_prot_except_view{trans_exc}.get<"asce_identifier">() == cpu::teid::teid_asce_id::ar_mode) {
                const u8 ar_num = lc->get<R"(exception_address_id\access_register_num)">();
                if (ar_num < 16U && frame->access_regs[ar_num] > 2U) {
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
