/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/stacktrace.cxxm
/// @brief s390x backchain-based stack unwinder with zxallsyms symbol resolution.

module arch.s390x.lib.stacktrace.core;
import zxfoundation.base.types;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.zxallsyms.core;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.types;

namespace arch::s390x::lib::stacktrace {

    [[nodiscard]] auto get_stack_info(u64 sp) noexcept -> stack_info {
        auto* lc = arch::s390x::cpu::lowcore::get_current();

        if (lc->get<"kernel_stack">() != 0) {
            const u64 top  = lc->get<"kernel_stack">();
            const u64 base = top - STACK_SIZE;
            if (sp >= base && sp < top)
                return { stack_type::task, base, top };
        }

        if (lc->get<"async_stack">() != 0) {
            const u64 top  = lc->get<"async_stack">();
            const u64 base = top - ASYNC_STACK_SIZE;
            if (sp >= base && sp < top)
                return { stack_type::async, base, top };
        }

        if (lc->get<"mcck_stack">() != 0) {
            const u64 top  = lc->get<"mcck_stack">();
            const u64 base = top - MCCK_STACK_SIZE;
            if (sp >= base && sp < top)
                return { stack_type::mcck, base, top };
        }

        return {};
    }

    [[nodiscard]] auto on_stack(u64 sp, const stack_info& info) noexcept -> bool {
        return info.type != stack_type::unknown
            && sp >= info.begin && sp < info.end;
    }


    [[nodiscard]] auto save_stack_trace_at(
        u64 sp, trace_entry* buf, u32 max_entries
    ) noexcept -> u32 {
        u32 depth = 0;

        while (sp != 0 && depth < max_entries) {
            if ((sp & 0x7) != 0)
                break;

            const auto info = get_stack_info(sp);
            if (info.type == stack_type::unknown)
                break;
            if (sp + STACK_FRAME_SIZE > info.end)
                break;

            const auto* frame = reinterpret_cast<const stack_frame*>(sp);
            const u64 ret_addr = frame->gprs[8]; // r14 at ABI offset 112

            if (ret_addr != 0) {
                auto sym_r = zxfoundation::sys::zxallsyms::lookup_by_addr(ret_addr);
                if (sym_r) {
                    buf[depth] = { ret_addr, sym_r->address, sym_r->name };
                } else {
                    buf[depth] = { ret_addr, 0, nullptr };
                }
                ++depth;
            }
            const u64 next_sp = frame->back_chain;
            if (next_sp == 0) break;
            if (next_sp <= sp) break; // must go UP (stack grows down)
            sp = next_sp;
        }

        return depth;
    }

    [[nodiscard]] auto save_stack_trace(
        trace_entry* buf, u32 max_entries
    ) noexcept -> u32 {
        return save_stack_trace_at(
            arch::s390x::cpu::processor::get_current_sp(), buf, max_entries);
    }

    /// @brief Print a backtrace starting from a given SP.
    auto dump_stack_at(u64 sp) noexcept -> void {
        using zxfoundation::sys::printk::pinfo;
        using zxfoundation::sys::printk::pnotice;

        trace_entry entries[MAX_TRACE_DEPTH];
        const u32 count = save_stack_trace_at(sp, entries, MAX_TRACE_DEPTH);

        pinfo("trace: call Trace:");
        for (u32 i = 0; i < count; ++i) {
            const auto& e = entries[i];
            if (e.sym_name != nullptr) {
                pinfo("   [{:#018x}] {}+{:#x}", e.addr, e.sym_name, e.addr - e.sym_addr);
            } else {
                pinfo("   [{:#018x}] <unknown>", e.addr);
            }
        }

        if (count == 0) {
            pnotice("   <no backtrace available>");
        }
    }

    /// @brief Print a backtrace from an IRQ frame (interrupt/pgm-check context).
    auto dump_stack_from_irq(
        const arch::s390x::cpu::irq::irq_frame* frame
    ) noexcept -> void {
        using zxfoundation::sys::printk::pinfo;
        using zxfoundation::sys::printk::pnotice;

        trace_entry entries[MAX_TRACE_DEPTH];
        u32 depth = 0;

        const u64 first_ret = frame->gprs[14];
        const u64 sp        = frame->gprs[15];

        if (first_ret != 0 && depth < MAX_TRACE_DEPTH) {
            auto sym_r = zxfoundation::sys::zxallsyms::lookup_by_addr(first_ret);
            if (sym_r) {
                entries[depth] = { first_ret, sym_r->address, sym_r->name };
            } else {
                entries[depth] = { first_ret, 0, nullptr };
            }
            ++depth;
        }

        depth += save_stack_trace_at(sp, entries + depth, MAX_TRACE_DEPTH - depth);

        pinfo("trace: call trace");
        for (u32 i = 0; i < depth; ++i) {
            const auto& e = entries[i];
            if (e.sym_name != nullptr) {
                pinfo("   [{:#018x}] {}+{:#x}", e.addr, e.sym_name, e.addr - e.sym_addr);
            } else {
                pinfo("   [{:#018x}] <unknown>", e.addr);
            }
        }

        if (depth == 0) {
            pnotice("   <no backtrace available>");
        }
    }

    /// @brief Print a backtrace from the current SP.
    auto dump_stack() noexcept -> void {
        dump_stack_at(arch::s390x::cpu::processor::get_current_sp());
    }

} // namespace zxfoundation::sys::stacktrace
