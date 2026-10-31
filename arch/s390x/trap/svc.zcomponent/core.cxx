/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/trap/svc.zcomponent/core.cxx
/// @brief Centralized SVC dispatch.

module arch.s390x.trap.svc.core;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.psw;
import zxfoundation.dgp.domain.core;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.pcss.core;
import zxfoundation.dgp.gate.types;
import zxfoundation.dgp.svc;
import zxfoundation.dgp.domain.types;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.sys.printk.core;

using zxfoundation::dgp::gate::call_namespace;
using zxfoundation::dgp::gate::sys_op;
using zxfoundation::dgp::svc::dispatch;
using zxfoundation::base::lifecycle_state;

namespace arch::s390x::trap::svc {

    /// @brief Hand a derived ALET from the kernel-side uregs into the live
    ///        user frame (shm_import / shm_map AR hand-off).
    /// @param[in,out] frame   Interrupt frame to patch (user ARs).
    /// @param[in]     result  AR index returned by the shm op (0..15).
    auto handoff_alet(cpu::irq::irq_frame* frame, const u64 result) noexcept -> void {
        if (result >= 16U) return;
        auto* strand = reinterpret_cast<zxfoundation::dgp::domain::strand*>(
            cpu::percpu::this_cpu().current_strand);
        if (strand && strand->body) {
            frame->access_regs[result] = strand->body->uregs.access_regs[result];
        }
    }

    /// @brief Centralized SVC nucleus-call dispatcher.
    [[nodiscard]] auto handle(cpu::irq::irq_frame* frame, const u16 svc_code) noexcept -> u64 {
        zxfoundation::dgp::domain::strand_note_interrupt_context(
            (frame->psw_mask & arch::s390x::cpu::psw::PSW_BIT_PSTATE) != 0);
        {
            auto& pcp = cpu::percpu::this_cpu();
            u64 cr1 = 0;
            u64 cr4 = 0;
            cpu::ctlreg::crread<1, 1>(cr1);
            cpu::ctlreg::crread<4, 4>(cr4);
            pcp.attached.publish_space(
                static_cast<u16>(cr4 & 0xFFFFULL),
                static_cast<u8>(cr1 != pcp.attached.asce ? 1U : 0U));
        }
        const u32 packed_nr = svc_code;
        const u64 a0 = frame->gprs[2];
        const u64 a1 = frame->gprs[3];
        const u64 a2 = frame->gprs[4];
        const u64 a3 = frame->gprs[5];
        const u64 a4 = frame->gprs[6];

        const auto* lc = cpu::lowcore::get_current();
        if (const auto* d  = lc ? reinterpret_cast<zxfoundation::dgp::domain::domain*>(lc->get<"current_domain">()) : nullptr;
            !d || d->ko.state != lifecycle_state::active) {
            return ~0ULL;
        }

        const u64 result = dispatch(packed_nr, a0, a1, a2, a3, a4);
        const u32 shm_import = zxfoundation::dgp::gate::pack_call(
            call_namespace::sys, static_cast<u32>(sys_op::shm_import));
        const u32 shm_map = zxfoundation::dgp::gate::pack_call(
            call_namespace::sys, static_cast<u32>(sys_op::shm_map));
        const u64 import_ar = result & 0xFFFFFFFFULL;
        if ((packed_nr == shm_import || packed_nr == shm_map)
            && result != ~0ULL && import_ar < 16U) {
            if (svc_code == 236U || svc_code == 237U) {
                handoff_alet(frame, import_ar);
            }
        }
        auto* strand = reinterpret_cast<zxfoundation::dgp::domain::strand*>(
            cpu::percpu::this_cpu().current_strand);
        if (strand != nullptr && strand->body != nullptr &&
            strand->body->gate_defer_entry != 0U) {
            const u64 entry_base = strand->body->gate_defer_entry;
            strand->body->gate_defer_entry = 0U;
            const auto* caller_dom = static_cast<const zxfoundation::dgp::domain::domain*>(
                strand->owner);
            cpu::pcss::state_entry_synthesize_return(
                *frame, strand->body->linkage_stack, entry_base,
                caller_dom ? caller_dom->asce.val : 0ULL);
            strand->body->in_pc = 0U;
            return frame->gprs[2];
        }

        if (strand != nullptr && strand->body != nullptr &&
            strand->body->signal_return_kind != 0U) {
            auto* b = strand->body;
            b->signal_saved_psw_mask = frame->psw_mask;
            b->signal_saved_psw_addr = frame->psw_addr;
            for (u32 i = 0; i < 16; ++i) {
                b->signal_saved_gprs[i] = frame->gprs[i];
                b->signal_saved_ars[i] = frame->access_regs[i];
                b->signal_saved_vrs[i][0] = frame->vector_regs_lo[i][0];
                b->signal_saved_vrs[i][1] = frame->vector_regs_lo[i][1];
                b->signal_saved_vrs[16 + i][0] = frame->vector_regs_hi[i][0];
                b->signal_saved_vrs[16 + i][1] = frame->vector_regs_hi[i][1];
            }
            b->signal_saved_fpc = frame->fpc;
            const u64 kind = b->signal_return_kind;
            const u64 payload = b->signal_payload;
            const u64 user_ctx = b->signal_user_ctx;
            b->signal_return_kind = 0U;
            frame->psw_addr = b->signal_handler_va;
            const u64 interrupted_r15 = frame->gprs[15];
            for (u32 i = 0; i < 16; ++i) {
                frame->gprs[i] = 0;
            }
            frame->gprs[2] = kind;
            frame->gprs[3] = payload;
            frame->gprs[4] = user_ctx;
            frame->gprs[15] = interrupted_r15;
            return kind;    // unused; frame is diverted above
        }

        if (result == 0xFFFF'FFFF'FFFF'FFF1ULL && strand != nullptr &&
            strand->body != nullptr &&
            strand->body->signal_saved_psw_addr != 0ULL) {
            auto* b = strand->body;
            frame->psw_mask = b->signal_saved_psw_mask;
            frame->psw_addr = b->signal_saved_psw_addr;
            for (u32 i = 0; i < 16; ++i) {
                frame->gprs[i] = b->signal_saved_gprs[i];
                frame->access_regs[i] = b->signal_saved_ars[i];
                frame->vector_regs_lo[i][0] = b->signal_saved_vrs[i][0];
                frame->vector_regs_lo[i][1] = b->signal_saved_vrs[i][1];
                frame->vector_regs_hi[i][0] = b->signal_saved_vrs[16 + i][0];
                frame->vector_regs_hi[i][1] = b->signal_saved_vrs[16 + i][1];
            }
            frame->fpc = b->signal_saved_fpc;
            b->signal_saved_psw_addr = 0ULL;
            return frame->gprs[2];
        }
        if (result == 0xFFFF'FFFF'FFFF'FFFEULL) {
            auto* strand = reinterpret_cast<zxfoundation::dgp::domain::strand*>(
                arch::s390x::cpu::percpu::this_cpu().current_strand);
            if (strand != nullptr && strand->body != nullptr) {
                const u64 entry_base =
                    strand->body->linkage_stack.cr15 - 288U;
                const auto* caller_dom =
                    static_cast<const zxfoundation::dgp::domain::domain*>(
                        strand->owner);
                arch::s390x::cpu::pcss::state_entry_synthesize_return(
                    *frame, strand->body->linkage_stack, entry_base,
                    caller_dom ? caller_dom->asce.val : 0ULL);
                strand->body->in_pc = 0U;
                const u64 restored = frame->gprs[2];
                return restored;
            }
        }
        return result;
    }

} // namespace arch::s390x::trap::svc
