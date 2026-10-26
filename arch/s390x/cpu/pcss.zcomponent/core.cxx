/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/pcss.zcomponent/core.cxx
/// @brief PC-ss substrate implementation.

module arch.s390x.cpu.pcss.core;
import arch.s390x.cpu.pcss.types;
import arch.s390x.cpu.aste;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.ctlreg;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.features;
import zxfoundation.memory.pmm;
import zxfoundation.sys.printk.core;
import zxfoundation.memory.hhdm;
import zxfoundation.base.types;
import zxfoundation.memory.pmm.types;
import lib.error;
import lib.cstring;
import lib.stl;

namespace arch::s390x::cpu::pcss {

    using zxfoundation::memory::pmm::alloc;
    using zxfoundation::memory::pmm::folio_active;
    using zxfoundation::memory::pmm::zone;
    using zxfoundation::memory::pmm::purpose;
    using zxfoundation::memory::pmm::page_init;

    namespace detail {

        constexpr u32 LT_ORDER = 2U;   // 4096 entries * 4 B = 16 KiB
        constexpr u32 ET_ORDER = 1U;   // 256 entries * 32 B = 8 KiB
        constexpr u8  LTL_16K = 127U;  // (LTL+1) * 128 B = 16 KiB
        constexpr u8  LFTL_4K = 15U;   // (LFTL+1) * 256 B = 4 KiB (LFT form)
        constexpr u32 LST_SLOTS = 32U; ///< 5-bit LSI (EA bits 51-55)
        constexpr u8  ETL_8K  = 63U;   // (ETL+1) * 128 B = 8 KiB

        [[nodiscard]] auto virt(const u64 phys) noexcept -> void* {
            return reinterpret_cast<void*>(zxfoundation::memory::hhdm::phys_to_virt(phys));
        }

    } // namespace detail

    /// @brief True when the ASN-and-LX-reuse form is available (STFLE bit 6)
    [[nodiscard]] auto pcss_reuse_enabled() noexcept -> bool {
        return features::has(features::facility::asn_lx_reuse);
    }

    auto tables_init(aste::aste& primary, domain_tables& t) noexcept
        -> std::expected<void, lib::kernel_error> {
        auto lt = alloc(detail::LT_ORDER, zone::dma31,
            purpose::page_table, page_init::zeroed);
        if (!lt) return std::unexpected(lt.error());
        auto et = alloc(detail::ET_ORDER, zone::dma31,
            purpose::page_table, page_init::zeroed);
        if (!et) {
            discard_value zxfoundation::memory::pmm::free(folio_active{
                lt.value().phys_base, detail::LT_ORDER,
                purpose::page_table, zone::dma31});
            return std::unexpected(et.error());
        }
        t.lt_phys = lt.value().phys_base;
        t.et_phys = et.value().phys_base;
        t.lt = static_cast<lt_entry*>(detail::virt(t.lt_phys));
        t.et = static_cast<et_entry*>(detail::virt(t.et_phys));
        t.et_used = 0U;

        auto lft = alloc(0U, zone::dma31, purpose::page_table, page_init::zeroed);
        if (lft) {
            t.lft_phys = lft.value().phys_base;
            auto lst = alloc(0U, zone::dma31, purpose::page_table, page_init::zeroed);
            if (lst) {
                t.lst_phys = lst.value().phys_base;
                auto* fte = static_cast<linkage_first_entry*>(detail::virt(t.lft_phys));
                fte[0].set<R"(word\lst_origin)">(static_cast<u32>(t.lst_phys >> 8U));
                auto* ste = static_cast<linkage_second_entry*>(detail::virt(t.lst_phys));
                ste[1].set<R"(word\entry_table_origin)">(static_cast<u32>(t.et_phys >> 6U));
                ste[1].set<R"(word\entry_table_length)">(detail::ETL_8K);
            }
        }

        primary.set_ltd_valid(true);
        if (pcss_reuse_enabled() && t.lft_phys != 0U) {
            primary.set_lftd(t.lft_phys, detail::LFTL_4K);
        } else {
            primary.set_ltd_origin(t.lt_phys);
            primary.set_ltd_length(detail::LTL_16K);
        }
        return {};
    }

    auto gate_materialize(
        domain_tables& owner, domain_tables& caller,
        const gate_request& req, pc_number& out) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (req.asn == 0U) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }

        if ((req.entry_va & 1ULL) != 0U) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        if (!req.extended_addressing) {
            const u64 limit = req.mode_31bit ? (1ULL << 31) : (1ULL << 24);
            if (req.entry_va >= limit) {
                return std::unexpected(lib::kernel_error::from_generic(
                    lib::generic_error::invalid_arg));
            }
        }

        u32 ex = 0U;
        for (; ex < ET_MAX_ENTRIES; ++ex) {
            const auto& e = owner.et[ex];
            if (e.get<"asn">() == 0U && e.get<R"(word1\entry_instruction_address)">() == 0U) break;
        }
        if (ex == ET_MAX_ENTRIES) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::out_of_range));
        }
        auto& ete = owner.et[ex];
        u32 lx = 0U;
        if (!pcss_reuse_enabled()) {
            for (; lx < LT_MAX_ENTRIES; ++lx) {
                if (caller.lt[lx].get<R"(word\entry_table_origin)">() == 0U) break;
            }
            if (lx == LT_MAX_ENTRIES) {
                return std::unexpected(lib::kernel_error::from_generic(
                    lib::generic_error::out_of_range));
            }
        } else if (caller.lst_used >= detail::LST_SLOTS || caller.lst_phys == 0U) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::out_of_range));
        }
        auto& lte = caller.lt[lx];

        ete.set<R"(word1\entry_instruction_address)">(req.entry_va >> 1);
        if (!req.extended_addressing) {
            ete.set<R"(word1\entry_basic_addressing_mode)">(req.mode_31bit);
        }
        ete.set<R"(word1\entry_problem_state)">(req.problem_state);
        ete.set<"authorization_key_mask">(req.ekm);
        ete.set<"asn">(req.asn);
        ete.set<"entry_key_mask">(req.entry_key_mask);
        ete.set<R"(stack_word\stacking)">(true);
        ete.set<R"(stack_word\extended_addressing)">(req.extended_addressing);
        ete.set<R"(stack_word\replace_psw_key)">(true);
        ete.set<R"(stack_word\replace_pkm)">(true);
        ete.set<R"(stack_word\entry_ar_mode)">(true);
        ete.set<R"(stack_word\sasn_new_pasn)">(true);
        ete.set<R"(stack_word\entry_key)">(static_cast<u8>(req.entry_key & 0x0FU));
        ete.set<"entry_parameter">(req.entry_param);
        ete.set<"aste_origin">(
            static_cast<u32>(owner.aste_phys & 0x7FFFFFC0ULL));

        ++owner.et_used;
        if (!pcss_reuse_enabled()) {
            lte.set<R"(word\entry_table_origin)">(
                static_cast<u32>(owner.et_phys >> 6U));
            lte.set<R"(word\etl)">(detail::ETL_8K);
            out = pc_number{(static_cast<u32>(lx) << 8U)
                          | static_cast<u32>(ex)};
            [[maybe_unused]] const auto walk =
                pc_number_walk(out, caller, 0U, false);
            return {};
        }

        auto* fte = static_cast<linkage_first_entry*>(
            detail::virt(caller.lft_phys));
        fte[0].set<R"(word\lst_origin)">(
            static_cast<u32>(caller.lst_phys >> 8U));
        auto* ste = static_cast<linkage_second_entry*>(
            detail::virt(caller.lst_phys));
        auto& lste = ste[caller.lst_used];
        lste.set<R"(word\entry_table_origin)">(
            static_cast<u32>(owner.et_phys >> 6U));
        lste.set<R"(word\entry_table_length)">(detail::ETL_8K);
        lste.set<"lstesn">(0U);
        const u32 lsi = caller.lst_used;
        ++caller.lst_used;
        out = pc_number{static_cast<u32>(ex)
                      | (static_cast<u32>(lsi) << 8U)};
        [[maybe_unused]] const auto walk =
            pc_number_walk(out, caller, 0U, true);
        return {};
    }

    auto pc_number_walk(
        pc_number nr, const domain_tables& t,
        const u32 gr15_low32, const bool reuse) noexcept
        -> std::expected<translation, lib::kernel_error> {
        const auto bad = [] {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        };
        const u32 raw = nr.underlying();
        const u32 ex = raw & 0xFFU;
        u64 eto = 0;
        u8 etl = 0;
        u32 lstesn = 0;
        if (!reuse) {
            const u32 lx = (raw >> 8U) & 0xFFFU;   // LX: bits 8-19
            if (t.lt == nullptr || lx >= LT_MAX_ENTRIES) return bad();
            if (static_cast<u8>(lx >> 5) > detail::LTL_16K) return bad();
            const auto& lte = t.lt[lx];
            if (lte.get<R"(word\reserved_0)">() != 0U) return bad();
            eto = static_cast<u64>(lte.get<R"(word\entry_table_origin)">()) << 6U;
            etl = lte.get<R"(word\etl)">();
        } else {
            if (t.lft_phys == 0 || t.lst_phys == 0) return bad();
            const u32 lfx = (raw >> 13U) & 0x3FU;  // LFX2: bits 13-18
            const u32 lsx = (raw >> 8U) & 0x1FU;   // LSX: bits 8-12
            const auto* fte = static_cast<const linkage_first_entry*>(
                detail::virt(t.lft_phys));
            if (fte[lfx].get<R"(word\invalid)">() != 0U) return bad();
            const auto* ste = static_cast<const linkage_second_entry*>(
                detail::virt(static_cast<u64>(fte[lfx].get<R"(word\lst_origin)">()) << 8U));
            if (ste[lsx].get<R"(word\invalid)">() != 0U) return bad();
            eto = static_cast<u64>(ste[lsx].get<R"(word\entry_table_origin)">()) << 6U;
            etl = ste[lsx].get<R"(word\entry_table_length)">();
            lstesn = ste[lsx].get<"lstesn">();
            if (lstesn != 0U && lstesn != gr15_low32) return bad();
        }
        if (static_cast<u8>(ex >> 2) > etl) return bad();
        translation tr;
        tr.ete_phys = eto + static_cast<u64>(ex) * ET_ENTRY_SIZE;
        tr.ete = static_cast<const et_entry*>(detail::virt(tr.ete_phys));
        tr.ex = ex;
        tr.lstesn = lstesn;
        return tr;
    }

    auto program_transfer(
        const domain_tables& t, pc_number nr, const u32 gr15_low32) noexcept
        -> std::expected<translation, lib::kernel_error> {
        return pc_number_walk(nr, t, gr15_low32, pcss_reuse_enabled());
    }

    auto program_transfer_instance(
        const domain_tables& t, pc_number nr,
        const u32 gr15_low32, const u32 instance) noexcept
        -> std::expected<translation, lib::kernel_error> {
        auto r = pc_number_walk(nr, t, gr15_low32, pcss_reuse_enabled());
        if (!r) return r;
        const auto* a = static_cast<const aste::aste*>(detail::virt(t.aste_phys));
        if (a->get<"astein">() != instance) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::invalid_arg));
        }
        return r;
    }

    auto gate_revoke(domain_tables& t, pc_number nr) noexcept -> void {
        const u32 ex = nr.underlying() & 0xFFU;
        if (ex < ET_MAX_ENTRIES) {
            auto& ete = t.et[ex];
            ete.set<"asn">(0U);
            ete.set<R"(word1\entry_instruction_address)">(0U);
        }
    }

    auto stack_init(stack& s) noexcept
        -> std::expected<void, lib::kernel_error> {
        auto sec = alloc(0U, zone::dma31, purpose::stack, page_init::zeroed);
        if (!sec) return std::unexpected(sec.error());
        s.sections[0].base_phys = sec.value().phys_base;
        s.sections[0].base_va =
            reinterpret_cast<u64>(detail::virt(sec.value().phys_base));
        s.section_count = 1U;
        s.next_section_id = 1U;

        header_entry header{};
        header.set<R"(backward\valid)">(false);
        header.set<R"(descriptor\word\entry_type)">(STACK_ET_HEADER);
        header.set<R"(descriptor\word\section_id)">(1U);
        header.set<R"(descriptor\word\remaining_free_space)">(
            static_cast<u16>(STACK_SECTION_SIZE - 2U * STACK_HEADER_SIZE));
        header.set<R"(descriptor\word\next_entry_size)">(
            static_cast<u16>(STACK_STATE_SIZE));

        auto* base = reinterpret_cast<u8*>(s.sections[0].base_va);
        for (usize i = 0; i < sizeof(header_entry); ++i)
            base[i] = header.as_bytes()[i];

        trailer_entry trailer{};
        // Last (only) section: no following section (V=0, address 0).
        trailer.set<R"(forward\valid)">(false);
        trailer.set<R"(descriptor\word\entry_type)">(STACK_ET_TRAILER);
        trailer.set<R"(descriptor\word\section_id)">(1U);
        auto* tbase = base + STACK_SECTION_SIZE - STACK_TRAILER_SIZE;
        for (usize i = 0; i < sizeof(trailer_entry); ++i)
            tbase[i] = trailer.as_bytes()[i];

        // §5.12 addressing: CR15 and the section links carry IDENTITY
        // home VAs (VA == phys).  The zxfl loader wires the same R3
        // tables at region-first index 0 (identity) and at the HHDM
        // index, but the 61-bit LSTE/LSHE link fields cannot represent
        // an HHDM VA whose top bits are set — a chained FSHA silently
        // lost 0xFFFF800000000000 and the hardware walked a garbage
        // address.  Kernel-side dereferences go through the HHDM alias
        // (hhdm::phys_to_virt).
        s.cr15 = s.sections[0].base_phys + STACK_HEADER_SIZE - STACK_DESC_SIZE;
        return {};
    }

    auto stack_save(stack& s, const u64 cr15_value) noexcept -> void {
        s.cr15 = cr15_value;
    }

    [[nodiscard]] auto stack_load(const stack& s) noexcept -> u64 {
        return s.cr15;
    }

    auto stack_grow(stack& s) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (s.section_count >= STACK_MAX_SECTIONS) {
            return std::unexpected(lib::kernel_error::from_generic(
                lib::generic_error::out_of_range));
        }
        auto sec = alloc(0U, zone::dma31, purpose::stack, page_init::zeroed);
        if (!sec) return std::unexpected(sec.error());

        const u8 sid = s.next_section_id;
        s.sections[s.section_count].base_phys = sec.value().phys_base;
        s.sections[s.section_count].base_va =
            reinterpret_cast<u64>(detail::virt(sec.value().phys_base));

        header_entry header{};
        header.set<R"(backward\valid)">(false);
        header.set<R"(descriptor\word\entry_type)">(STACK_ET_HEADER);
        header.set<R"(descriptor\word\section_id)">(sid);
        header.set<R"(descriptor\word\remaining_free_space)">(
            static_cast<u16>(STACK_SECTION_SIZE - 2U * STACK_HEADER_SIZE));
        header.set<R"(descriptor\word\next_entry_size)">(
            static_cast<u16>(STACK_STATE_SIZE));

        auto* base = reinterpret_cast<u8*>(s.sections[s.section_count].base_va);
        for (usize i = 0; i < sizeof(header_entry); ++i)
            base[i] = header.as_bytes()[i];

        trailer_entry trailer{};
        trailer.set<R"(forward\valid)">(false);   // new last section
        trailer.set<R"(descriptor\word\entry_type)">(STACK_ET_TRAILER);
        trailer.set<R"(descriptor\word\section_id)">(sid);
        auto* tbase = base + STACK_SECTION_SIZE - STACK_TRAILER_SIZE;
        for (usize i = 0; i < sizeof(trailer_entry); ++i)
            tbase[i] = trailer.as_bytes()[i];

        if (s.section_count == 0U) {
            s.cr15 = s.sections[0].base_phys + STACK_HEADER_SIZE
                     - STACK_DESC_SIZE;
        } else {
            auto* prev_trailer = reinterpret_cast<trailer_entry*>(
                s.sections[s.section_count - 1U].base_va
                + STACK_SECTION_SIZE - STACK_TRAILER_SIZE);
            // §5.12 wire format: the LSTE doubleword IS the raw home
            // VA (low three bits zero) with V at bit 63-LSB; the msb0
            // bits<0,60> field occupies the top 61 bits, so it stores
            // VA >> 3.  Passing the raw VA made the hardware walk
            // VA*8 (the 13:22/13:50 captures' 0x1F3E30040 fault).
            const u64 next_header_desc =
                s.sections[s.section_count].base_phys + STACK_HEADER_SIZE
                - STACK_DESC_SIZE;
            prev_trailer->set<R"(forward\address)">(next_header_desc >> 3U);
            prev_trailer->set<R"(forward\valid)">(true);
        }

        ++s.section_count;
        s.next_section_id = static_cast<u8>(sid + 1U);
        return {};
    }

    [[nodiscard]] auto stack_forensics(const stack& s) noexcept -> bool {
        for (u32 i = 0; i < s.section_count; ++i) {
            const auto* base =
                reinterpret_cast<const u8*>(s.sections[i].base_va);
            auto header =
                reinterpret_cast<const header_entry*>(base);
            if (header->get<R"(descriptor\word\entry_type)">() != STACK_ET_HEADER)
                return false;
            auto trailer =
                reinterpret_cast<const trailer_entry*>(
                    base + STACK_SECTION_SIZE - STACK_TRAILER_SIZE);
            if (trailer->get<R"(descriptor\word\entry_type)">() != STACK_ET_TRAILER)
                return false;
            u64 off = STACK_HEADER_SIZE;
            u32 guard = 0U;
            while (off + STACK_DESC_SIZE <= STACK_SECTION_SIZE - STACK_TRAILER_SIZE) {
                auto d =
                    reinterpret_cast<const entry_descriptor*>(base + off);
                const u8 et = d->get<R"(word\entry_type)">();
                if (et != STACK_ET_STATE && et != STACK_ET_HEADER)
                    return false;
                const u16 nes = d->get<R"(word\next_entry_size)">();
                if (nes == 0U) break;
                off += nes;
                if (++guard > STACK_STATE_PER_SECTION) return false;
            }
        }
        return true;
    }

    auto stack_release(stack& s) noexcept -> void {
        for (u32 i = 0; i < s.section_count; ++i) {
            discard_value zxfoundation::memory::pmm::free(folio_active{
                s.sections[i].base_phys, 0U,
                purpose::stack, zone::dma31});
        }
        s.section_count = 0U;
        s.cr15 = 0U;
    }

    namespace detail {

        constexpr u32 STATE_OFF_GR   = 0U;
        constexpr u32 STATE_OFF_PKM  = 128U;
        constexpr u32 STATE_OFF_SASN = 130U;
        constexpr u32 STATE_OFF_PASN = 134U;
        constexpr u32 STATE_OFF_PSW1 = 136U;
        constexpr u32 STATE_OFF_PSW2 = 168U;
        constexpr u32 STATE_OFF_AR   = 224U;
        constexpr u32 STATE_OFF_DESC = 288U;
        constexpr u8  LSED_UET_U     = 0x80U;

        [[nodiscard]] inline auto rd64(const u8* e, u32 off) noexcept -> u64 {
            u64 v = 0;
            lib::memcpy(&v, e + off, sizeof(v));
            return v;
        }

        [[nodiscard]] inline auto rd32(const u8* e, u32 off) noexcept -> u32 {
            u32 v = 0;
            lib::memcpy(&v, e + off, sizeof(v));
            return v;
        }

        [[nodiscard]] inline auto rd16(const u8* e, u32 off) noexcept -> u16 {
            u16 v = 0;
            lib::memcpy(&v, e + off, sizeof(v));
            return v;
        }

    } // namespace detail

    [[nodiscard]] auto state_entry_restore(
        cpu::irq::irq_frame& frame, u64 entry_base) noexcept -> bool {
        // entry_base is a CR15-derived IDENTITY home VA; the kernel
        // reads the entry through the HHDM alias.
        const u8* entry = reinterpret_cast<const u8*>(
            zxfoundation::memory::hhdm::phys_to_virt(entry_base));
        if ((entry[detail::STATE_OFF_DESC] & 0x7FU) != STACK_ET_STATE)
            return false;
        for (u32 i = 2U; i <= 14U; ++i) {
            frame.gprs[i] = detail::rd64(entry, detail::STATE_OFF_GR + i * 8U);
            frame.access_regs[i] =
                detail::rd32(entry, detail::STATE_OFF_AR + i * 4U);
        }
        frame.psw_mask = detail::rd64(entry, detail::STATE_OFF_PSW1);
        frame.psw_addr = detail::rd64(entry, detail::STATE_OFF_PSW2);
        cpu::ctlreg::crwrite<3, 3>(
            (static_cast<u64>(detail::rd16(entry, detail::STATE_OFF_PKM)) << 16) |
            detail::rd16(entry, detail::STATE_OFF_SASN));
        cpu::ctlreg::crwrite<4, 4>(
            static_cast<u64>(detail::rd16(entry, detail::STATE_OFF_PASN)));
        return true;
    }

    auto state_entry_unstack(stack& s, u64 entry_base, u64 caller_asce) noexcept -> void {
        const u8* entry = reinterpret_cast<const u8*>(
            zxfoundation::memory::hhdm::phys_to_virt(entry_base));
        auto& pcp = cpu::percpu::this_cpu();
        // §5.12 BUG-072: CR1 must return to the CALLER's own space.  The
        // percpu attached record holds the LAST DISPATCHED owner's ASCE —
        // on a deferred-gate resume that is the serving worker's domain,
        // not the parked caller's.
        cpu::ctlreg::crwrite<1, 1>(caller_asce);
        cpu::ctlreg::crwrite<5, 5>(
            aste::aste_origin(static_cast<u32>(
                detail::rd16(entry, 134U))) & 0x7FFFFFC0ULL);
        const u64 prev = entry_base - 8U;
        s.cr15 = prev;
        cpu::ctlreg::crwrite<15, 15>(prev);
        pcp.attached.publish_space(
            detail::rd16(entry, detail::STATE_OFF_PASN), 0U);
    }

    auto state_entry_write_reply(
        u64 entry_base, u64 r0, u64 r1, u64 status) noexcept -> void {
        auto* entry = reinterpret_cast<volatile u8*>(
            zxfoundation::memory::hhdm::phys_to_virt(entry_base));
        auto* gr = reinterpret_cast<volatile u64*>(entry);
        entry[detail::STATE_OFF_DESC] = static_cast<u8>(
            entry[detail::STATE_OFF_DESC] | detail::LSED_UET_U);
        gr[0] = r0;
        gr[1] = r1;
        gr[15] = status;
        entry[detail::STATE_OFF_DESC] = static_cast<u8>(
            entry[detail::STATE_OFF_DESC] & ~detail::LSED_UET_U);
    }

    auto state_entry_synthesize_return(
        cpu::irq::irq_frame& frame, stack& s, u64 entry_base,
        u64 caller_asce) noexcept -> void {
        if (!state_entry_restore(frame, entry_base)) return;
        const u8* entry = reinterpret_cast<const u8*>(
            zxfoundation::memory::hhdm::phys_to_virt(entry_base));
        frame.gprs[0] = detail::rd64(entry, 0U);
        frame.gprs[1] = detail::rd64(entry, 8U);
        frame.gprs[15] = detail::rd64(entry, 120U);
        state_entry_unstack(s, entry_base, caller_asce);
    }

} // namespace arch::s390x::cpu::pcss
