/// SPDX-License-Identifier: Apache-2.0
/// @file fixup.cxxm
/// @brief Program-check fixup table

module arch.s390x.trap.fixup.core;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.processor;
import zxfoundation.base.types;
import zxfoundation.base.linksym;

#define __zxmacro_want_extable
#include macro_extensions

namespace arch::s390x::trap::fixup::detail {

    [[nodiscard]] auto base() noexcept -> const fixup_entry* {
        return reinterpret_cast<const fixup_entry*>(
            zxfoundation::base::linksym::__ex_table_start);
    }

    [[nodiscard]] auto end_ptr() noexcept -> const fixup_entry* {
        return reinterpret_cast<const fixup_entry*>(
            zxfoundation::base::linksym::__ex_table_end);
    }

    [[nodiscard]] auto count() noexcept -> usize {
        const auto* b = base();
        const auto* e = end_ptr();
        return (!b || !e || b >= e) ? 0 : static_cast<usize>(e - b);
    }

    [[nodiscard]] auto extract_reg(u16 data) noexcept -> u8 {
        return static_cast<u8>((data >> 12) & 0xF);
    }

} // namespace detail

namespace arch::s390x::trap::fixup {

    /// @brief Search exception table and apply entry-type-specific recovery.
    /// @param faulting_addr  PSW instruction address at fault.
    /// @param frame          Current interrupt frame (to patch saved GPRs).
    /// @return true if a fixup was applied, false otherwise.
    [[nodiscard]] auto try_applying_fixup(
        u64 faulting_addr,
        arch::s390x::cpu::irq::irq_frame* frame
    ) noexcept -> bool {
        const usize n = detail::count();
        if (n == 0) return false;

        const auto* entries = detail::base();
        const auto field_address = [](const fixup_entry* entry,
                                      usize offset,
                                      i32 delta) noexcept -> u64 {
            return reinterpret_cast<u64>(entry) + offset +
                   static_cast<i64>(delta);
        };

        for (usize i = 0; i < n; ++i) {
            const fixup_entry& e = entries[i];
            if (field_address(&e, 0, e.fault_delta) != faulting_addr)
                continue;

            auto& pcp = arch::s390x::cpu::percpu::this_cpu();
            pcp.pgm_fixup_applied = 1U;
            pcp.pgm_fixup_addr    = field_address(&e, 4, e.fixup_delta);

            switch (e.type) {
            case EX_TYPE_FIXUP:
                break;

            case EX_TYPE_UA_LOAD_REG: {
                const u8 reg = detail::extract_reg(e.data);
                auto* lc = arch::s390x::cpu::lowcore::get_current();
                const u64 fsa = lc->get<"failing_storage_address">();
                if (frame) {
                    frame->gprs[reg] = fsa;
                }
                break;
            }

            case EX_TYPE_UA_LOAD_RP: {
                const u8 reg = detail::extract_reg(e.data);
                if (frame) {
                    frame->gprs[reg]     = 0ULL;
                    frame->gprs[reg + 1] = 0ULL;
                }
                break;
            }

            case EX_TYPE_UA_STORE: {
                const u8 reg = detail::extract_reg(e.data);
                if (frame) {
                    frame->gprs[reg] = 0ULL;
                }
                break;
            }

            case EX_TYPE_UA_MVCOS_TO:
            case EX_TYPE_UA_MVCOS_FROM: {
                const u8 reg = detail::extract_reg(e.data);
                if (frame) {
                    frame->gprs[reg] = 0ULL;
                }
                break;
            }

            default:
                break;
            }
            return true;
        }
        return false;
    }

    auto clear() noexcept -> void {
        auto& pcp = arch::s390x::cpu::percpu::this_cpu();
        pcp.pgm_fixup_applied = 0U;
        pcp.pgm_fixup_addr    = 0U;
    }

    [[nodiscard]] auto was_applied() noexcept -> bool {
        return arch::s390x::cpu::percpu::this_cpu().pgm_fixup_applied != 0U;
    }

} // namespace arch::s390x::trap::fixup
