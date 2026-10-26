/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/strandctx.zcomponent/core.cxx
/// @brief Domain/strand context setup and user-mode first-entry trampolines.

module zxfoundation.sched.strandctx.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.base.types;
import zxfoundation.dgp.isolation;
import zxfoundation.sys.syschk.barebones;
import zxfoundation.dgp.isolation.types;
import zxfoundation.sys.syschk.core;
import arch.s390x.sched.types;
import arch.s390x.cpu.pcss.core;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.aste;
import arch.s390x.cpu.psw;
import lib.cstring;
import lib.stl;

#define __zxmacro_want_assert
#include macro_extensions

from_global(nomangle) {

    /// @brief Assembly stub: load ARs/VRs/FPC from @c user_regs, then jump to
    ///        @c ret_to_user.
    /// @param[in] %r2  frame_base — kernel stack slot with 16 user GPRs + PSW.
    /// @param[in] %r3  user_regs* — AR/VR/FPC snapshot to restore before LPSWE.
    [[noreturn]] auto __domain_enter_user_asm(u64 frame_base, arch::s390x::sched::user_regs* uregs) noexcept -> void;

    /// @brief Assembly kernel-thread first-entry trampoline.
    [[noreturn]] auto domain_trampoline() noexcept -> void;

    /// @brief Unified first-context-switch dispatcher (kernel-thread or
    ///        user-process).
    [[noreturn]] auto ret_from_fork() noexcept -> void;

    /// @brief Low-level context switch: (strand* prev, strand* next) -> domain* (prev->owner).
    auto __switch_to_asm(
        zxfoundation::dgp::domain::strand* prev,
        zxfoundation::dgp::domain::strand* next
    ) noexcept -> zxfoundation::dgp::domain::domain*;

}

namespace zxfoundation::sched::strandctx::detail {

    using arch::s390x::sched::user_regs;

    static constexpr u64 SWITCH_FRAME_SIZE     = 640;   ///< __switch_to_asm save area.
    static constexpr u64 TRAMPOLINE_FRAME_SIZE = 160;  ///< domain_trampoline's own frame.

    static constexpr u64 USER_RETURN_FRAME_SIZE = 0x120;

    static constexpr u64 SF_GPRS               = 48;   ///< callee-saved GPR save region start (in __switch_to_asm post-pop frame).

    static constexpr u64 USER_GPRS_OFF         = 0x90;  ///< user GPRs start within the user return frame.
    static constexpr u64 USER_PSW_OFF          = 0x110; ///< user PSW (mask+addr, 16B) within the user return frame.

    static constexpr u64 FRAME_ARS_OFF         = 0;    ///< AR bank within __switch_to_asm save area.
    static constexpr u64 FRAME_FPC_OFF         = 64;   ///< FPC within save area.
    static constexpr u64 FRAME_VRS_OFF         = 72;   ///< VR bank within save area.
    static constexpr u64 FRAME_VRS_LO_SIZE     = 256;  ///< v0-v15 (16 * 16 bytes).
    static constexpr u64 FRAME_VRS_HI_SIZE     = 256;  ///< v16-v31.

    static constexpr u64 UREGS_GPRS_OFF        = 0;                ///< gprs[16] start.
    static constexpr u64 UREGS_GPRS_SIZE        = 16U * 8U;         ///< 128 bytes.
    static constexpr u64 UREGS_PSW_MASK_OFF    = UREGS_GPRS_OFF + UREGS_GPRS_SIZE; ///< +0x80.
    static constexpr u64 UREGS_PSW_ADDR_OFF    = UREGS_PSW_MASK_OFF + 8U;          ///< +0x88.
    static constexpr u64 UREGS_ARS_OFF         = UREGS_PSW_ADDR_OFF + 8U;          ///< +0x90.
    static constexpr u64 UREGS_ARS_SIZE        = 16U * 4U;                          ///< 64 bytes.
    static constexpr u64 UREGS_FPC_OFF         = UREGS_ARS_OFF + UREGS_ARS_SIZE;    ///< +0xD0.
    static constexpr u64 UREGS_VRS_OFF         = UREGS_FPC_OFF + 8U;                ///< +0xD8.
    [[maybe_unused]] static constexpr u64 UREGS_VRS_SIZE = 32U * 16U;                ///< 512 bytes.

    /// @brief Apply the canonical user-PSW normalization
    /// @param[in,out] uregs  Mutable @c user_regs candidate PSW.
    /// @param[in]     owner  The owning domain (may be nullptr in tests);
    ///                       its @c storage_key is blended into PSW bits 52..55.
    auto normalize_user_psw(user_regs& uregs, const domain* owner) noexcept -> void {
        uregs.psw_mask &= ~arch::s390x::cpu::psw::PSW_ASC_HOME;
        uregs.psw_mask &= ~arch::s390x::cpu::psw::PSW_ASC_ACCREG;
        uregs.psw_mask |= arch::s390x::cpu::psw::PSW_ASC_PRIMARY;
        if (owner) {
            const u64 key_field =
                (static_cast<u64>(owner->storage_key) & 0xFULL)
                << arch::s390x::cpu::psw::PSW_KEY_SHIFT;
            uregs.psw_mask &=
                ~(static_cast<u64>(0xFULL) << arch::s390x::cpu::psw::PSW_KEY_SHIFT);
            uregs.psw_mask |= key_field;
        }
    }

    /// @brief Stage the user_regs AR/FPC/VR state
    /// @param s  Strand whose @c ksp IS already set to the bottom of the
    ///           save area
    /// @param uregs  Source user_regs (must be non-null).
    auto stage_user_ars_vrs_fpc(strand* s, const user_regs* uregs) noexcept -> void {
        const u64 ksp = s->ksp;
        // Access registers: 16 * u32 = 64 bytes.
        lib::memcpy(
            reinterpret_cast<void*>(ksp + FRAME_ARS_OFF),
            reinterpret_cast<const void*>(
                reinterpret_cast<u8*>(const_cast<user_regs*>(uregs)) + UREGS_ARS_OFF),
            UREGS_ARS_SIZE);
        // FPC: 4 bytes.
        lib::memcpy(
            reinterpret_cast<void*>(ksp + FRAME_FPC_OFF),
            reinterpret_cast<const void*>(
                reinterpret_cast<u8*>(const_cast<user_regs*>(uregs)) + UREGS_FPC_OFF),
            sizeof(u32));
        // Vector registers: v0-v15 at ksp+72, v16-v31 at ksp+72+256.
        lib::memcpy(
            reinterpret_cast<void*>(ksp + FRAME_VRS_OFF),
            reinterpret_cast<const void*>(
                reinterpret_cast<u8*>(const_cast<user_regs*>(uregs)) + UREGS_VRS_OFF),
            FRAME_VRS_LO_SIZE);
        lib::memcpy(
            reinterpret_cast<void*>(ksp + FRAME_VRS_OFF + FRAME_VRS_LO_SIZE),
            reinterpret_cast<const void*>(
                reinterpret_cast<u8*>(const_cast<user_regs*>(uregs)) + UREGS_VRS_OFF + FRAME_VRS_LO_SIZE),
            FRAME_VRS_HI_SIZE);
    }

} // namespace detail

namespace zxfoundation::sched::strandctx {

    using zxfoundation::dgp::domain::domain;
    using zxfoundation::dgp::domain::strand;
    using arch::s390x::sched::user_regs;

    /// @brief Set up the initial kernel stack frame for a strand so that the
    /// @param s      Target strand with valid @c kstack_top
    /// @param entry  Non-null kernel entry function
    /// @param arg    Opaque argument passed to @c entry in r2
    auto setup_strand_context_kernel_thread(
        strand* s,
        strand_entry_fn entry,
        void* arg
    ) noexcept -> void {
        if (!s || !entry) [[unlikely]] return;

        const u64 ksp = s->kstack_top - detail::SWITCH_FRAME_SIZE - detail::TRAMPOLINE_FRAME_SIZE;
        const u64 sp  = ksp + detail::SWITCH_FRAME_SIZE;
        const u64 gpr_area = sp + detail::SF_GPRS;

        std::fill_n(reinterpret_cast<u8*>(ksp), detail::SWITCH_FRAME_SIZE, u8{0});

        auto* frame = reinterpret_cast<u64*>(gpr_area);
        frame[0] = reinterpret_cast<u64>(arg);                 // r6  = arg
        frame[1] = reinterpret_cast<u64>(entry);               // r7  = entry fn (non-zero)
        frame[8] = reinterpret_cast<u64>(&ret_from_fork);      // r14 = unified dispatcher
        frame[9] = sp;                                          // r15 = sp for domain_trampoline
                                                              // (domain_trampoline adjusts again)

        s->ksp = ksp;
    }

    /// @brief Set up the initial kernel stack frame for a strand whose first
    ///        context switch performs an @c LPSWE into user mode.
    /// @param s      Target strand with valid @c kstack_top and @c body->uregs
    ///               already populated
    /// @param uregs  Pointer to the user_regs snapshot
    /// @param owner  The owning domain, used for ASC normalization and DGP
    ///               isolation validation.
    auto setup_strand_context_user(
        strand* s,
        const user_regs* uregs,
        const domain* owner
    ) noexcept -> void {
        if (!s || !s->body || !uregs) [[unlikely]] return;
        if (uregs->psw_addr == 0U) [[unlikely]] return;

        if (owner) {
            const auto admission = zxfoundation::dgp::isolation::validate_scheduler_admission(
                owner->ko.state, owner->root_phys, owner->asce.val);
            if (!admission) [[unlikely]] {
                sys::syschk::syschk_fatal(
                   admission.error(),
                   "sched: scheduler admission failed for strand creation: domain={}, state={}, root_phys={:#x}, asce={:#x}",
                   owner->ko.id, static_cast<u32>(owner->ko.state), owner->root_phys, owner->asce.val
                );
            }
        }

        user_regs& uregs_mut = s->body->uregs;
        if (&uregs_mut != uregs) {
            lib::memcpy(&uregs_mut, uregs, sizeof(user_regs));
        }
        detail::normalize_user_psw(uregs_mut, owner);

        const u64 user_return_base = s->kstack_top - detail::USER_RETURN_FRAME_SIZE;
        const u64 ksp = user_return_base - detail::SWITCH_FRAME_SIZE;

        std::fill_n(reinterpret_cast<u8*>(ksp), detail::SWITCH_FRAME_SIZE, u8{0});

        std::fill_n(
            reinterpret_cast<u8*>(user_return_base),
            detail::USER_GPRS_OFF,
            u8{0});

        // Provisionally assign ksp so the helper stages from s->ksp.
        s->ksp = ksp;

        detail::stage_user_ars_vrs_fpc(s, &uregs_mut);

        auto* user_gprs = reinterpret_cast<u64*>(user_return_base + detail::USER_GPRS_OFF);
        for (u32 i = 0U; i < 16U; ++i) {
            user_gprs[i] = uregs_mut.gprs[i];
        }
        auto* user_psw = reinterpret_cast<u64*>(user_return_base + detail::USER_PSW_OFF);
        user_psw[0] = uregs_mut.psw_mask;
        user_psw[1] = uregs_mut.psw_addr;

        auto* sf = reinterpret_cast<u64*>(user_return_base + detail::SF_GPRS);
        sf[0] = 0ULL;                                          // r6  = unused
        sf[1] = static_cast<u64>(-1LL);                        // r7  = USER_ENTRY_SENTINEL
        sf[8] = reinterpret_cast<u64>(&ret_from_fork);         // r14 = unified dispatcher
        sf[9] = user_return_base;                              // r15 = user return frame base

        s->ksp = ksp;
    }

    /// @brief Seed a domain primary's linkage stack (§5.12).
    /// @param[in,out] d  Domain whose primary strand gets the stack.
    /// @return Success, or the stack allocation error.
    /// @note CR15=0 means an architecturally empty linkage stack; any
    ///       PROGRAM CALL from such a strand faults with no recoverable
    ///       section.  Bootstrap-launched primaries must be seeded before
    ///       first entry, exactly as dom_exec seeds its children.
    [[nodiscard]] auto seed_primary_linkage_stack(
        domain* d) noexcept -> std::expected<void, lib::kernel_error> {
        if (!d || !d->primary_strand.body) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        return arch::s390x::cpu::pcss::stack_init(
            d->primary_strand.body->linkage_stack);
    }

    /// @brief Convenience wrapper — kernel-thread first-switch for the
    ///        primary strand of a domain.
    auto setup_primary_strand_context(
        domain* d,
        strand_entry_fn entry,
        void* arg
    ) noexcept -> void {
        setup_strand_context_kernel_thread(&d->primary_strand, entry, arg);
    }

    /// @brief Convenience wrapper — user-process first-switch for the
    ///        primary strand of a domain.
    auto setup_primary_strand_context_user(
        domain* d
    ) noexcept -> void {
        setup_strand_context_user(&d->primary_strand, &d->primary_strand.body->uregs, d);
    }

    /// @brief Enter user mode for a domain
    [[noreturn]] auto domain_enter_user(strand* s) noexcept -> void {
        auto* d = static_cast<domain*>(s->owner);
        auto* lc = arch::s390x::cpu::lowcore::get_current();

        if (d) {
            chk_on(!zxfoundation::dgp::isolation::validate_scheduler_admission(
                d->ko.state, d->root_phys, d->asce.val));
        }

        lc->set<"user_asce"> (d ? d->asce.val : 0);
        const u64 user_asce = d ? d->asce.val : 0;
        arch::s390x::cpu::ctlreg::crwrite<1, 1>(user_asce);
        arch::s390x::cpu::ctlreg::crwrite<15, 15>(
            arch::s390x::cpu::pcss::stack_load(s->body->linkage_stack));
        if (d) {
            const u64 rec = d->pc_tables.aste_phys & 0x7FFF'FFC0ULL;
            const u64 pasteo = (rec != 0ULL)
                ? rec
                : (arch::s390x::cpu::aste::aste_origin(d->pc_asn) & 0x7FFF'FFC0ULL);
            arch::s390x::cpu::ctlreg::crwrite<5, 5>(pasteo);
        }
        {
            u64 cr14 = 0;
            arch::s390x::cpu::ctlreg::crread<14, 14>(cr14);
            arch::s390x::cpu::ctlreg::crwrite<14, 14>(cr14 | 0x0008'0000ULL);
        }
        {
            static bool probed = false;
            if (!probed) {
                probed = true;
                u64 cr5 = 0, cr13 = 0, cr14 = 0;
                arch::s390x::cpu::ctlreg::crread<5, 5>(cr5);
                arch::s390x::cpu::ctlreg::crread<13, 13>(cr13);
                arch::s390x::cpu::ctlreg::crread<14, 14>(cr14);
            }
        }
        if (d) arch::s390x::cpu::aste::art_context_load(d->art);

        if (d) {
            s->body->uregs.psw_mask |=
                (static_cast<u64>(d->storage_key) << arch::s390x::cpu::psw::PSW_KEY_SHIFT);
            s->body->uregs.psw_mask &= ~arch::s390x::cpu::psw::PSW_ASC_HOME;
            s->body->uregs.psw_mask &= ~arch::s390x::cpu::psw::PSW_ASC_ACCREG;
            s->body->uregs.psw_mask |= arch::s390x::cpu::psw::PSW_ASC_PRIMARY;
        }

        static constexpr u64 RET_FRAME_SIZE = 0x90;
        const u64 frame_base = s->kstack_top - RET_FRAME_SIZE;
        auto* gprs = reinterpret_cast<u64*>(frame_base);
        auto* psw  = reinterpret_cast<u64*>(frame_base + 0x80);

        for (u32 i = 0; i < 16; ++i)
            gprs[i] = s->body->uregs.gprs[i];

        psw[0] = s->body->uregs.psw_mask;
        psw[1] = s->body->uregs.psw_addr;

        lc->set<"current_domain"> (reinterpret_cast<u64>(d));
        lc->set<"kernel_stack">   (s->kstack_top);
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        pcp.current_strand = reinterpret_cast<u64>(s);
        pcp.attached.publish(user_asce, reinterpret_cast<u64>(d),
            d ? d->ko.id : 0U, d ? static_cast<u8>(d->ko.state) : 0U);

        __domain_enter_user_asm(frame_base, &s->body->uregs);
    }

    /// @brief Overload taking domain* for backward compatibility.
    [[noreturn]] auto domain_enter_user(domain* d) noexcept -> void {
        domain_enter_user(&d->primary_strand);
    }

} // namespace zxfoundation::sched::strandctx
