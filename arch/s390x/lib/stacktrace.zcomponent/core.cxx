/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/lib/stacktrace.zcomponent/core.cxx
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

from_global(nomangle) {
    /// @brief Kernel-owned BSP boot stack (nucleus .bss)
    from_global(reference) u8 __kernel_boot_stack[];
}

namespace arch::s390x::lib::stacktrace {

    /// @brief Index into stack_frame::gprs[] for the saved r14 (offset 112).
    constexpr u32 GPRS_R14 = 8;

    using cpu::processor::stack_frame;
    using cpu::processor::STACK_SIZE;
    using cpu::processor::ASYNC_STACK_SIZE;
    using cpu::processor::MCCK_STACK_SIZE;
    using cpu::processor::NODAT_STACK_SIZE;
    using cpu::processor::STACK_FRAME_SIZE;

    /// @brief Test whether @p sp falls within [base, top) for one stack.
    static auto sp_in(u64 sp, u64 top, u64 size) noexcept -> bool {
        if (top == 0 || size == 0)
            return false;
        const u64 base = top - size;
        return sp >= base && sp < top;
    }

    /// @brief Resolve which kernel stack contains @p sp.
    [[nodiscard]] auto get_stack_info(u64 sp) noexcept -> stack_info {
        if (sp == 0) [[unlikely]]
            return {};

        const auto* lc = cpu::lowcore::get_current();
        if (lc != nullptr) {
            const u64 async_top = lc->get<"async_stack">();
            if (sp_in(sp, async_top, ASYNC_STACK_SIZE))
                return { stack_type::async, async_top - ASYNC_STACK_SIZE, async_top };

            const u64 mcck_top = lc->get<"mcck_stack">();
            if (sp_in(sp, mcck_top, MCCK_STACK_SIZE))
                return { stack_type::mcck, mcck_top - MCCK_STACK_SIZE, mcck_top };

            const u64 nodat_top = lc->get<"nodat_stack">();
            if (sp_in(sp, nodat_top, NODAT_STACK_SIZE))
                return { stack_type::nodat, nodat_top - NODAT_STACK_SIZE, nodat_top };
        }

        const u64 task_top = cpu::percpu::this_cpu().kernel_stack;
        if (sp_in(sp, task_top, STACK_SIZE))
            return { stack_type::task, task_top - STACK_SIZE, task_top };

        const u64 boot_top = reinterpret_cast<u64>(&__kernel_boot_stack[STACK_SIZE]);
        if (sp_in(sp, boot_top, STACK_SIZE))
            return { stack_type::task, boot_top - STACK_SIZE, boot_top };

        return {};
    }

    [[nodiscard]] auto on_stack(u64 sp, const stack_info& info) noexcept -> bool {
        if (info.type == stack_type::unknown)
            return false;
        if (sp < info.begin || (sp & 0x7) != 0)
            return false;
        return (sp + STACK_FRAME_SIZE) <= info.end;
    }

    /// @brief Record a single resolved return address into the trace buffer.
    static auto record_ip(u64 addr, trace_entry* buf, u32 slot) noexcept -> void {
        if (const auto sym_r = zxfoundation::sys::zxallsyms::lookup_by_addr(addr - 1)) {
            if (addr >= sym_r->address) {
                buf[slot] = { addr, sym_r->address, sym_r->name };
                return;
            }
        }
        buf[slot] = { addr, 0, nullptr };
    }

    /// @brief Record an exact interrupted address — no return-address shift.
    static auto record_ip_exact(u64 addr, trace_entry* buf, u32 slot) noexcept -> void {
        if (const auto sym_r = zxfoundation::sys::zxallsyms::lookup_by_addr(addr)) {
            buf[slot] = { addr, sym_r->address, sym_r->name };
        } else {
            buf[slot] = { addr, 0, nullptr };
        }
    }

    /// @brief Recover the return address of the frame at @p sp.
    static auto frame_return_addr(u64 sp) noexcept -> u64 {
        const auto* sf = reinterpret_cast<const stack_frame*>(sp);
        return sf->gprs[GPRS_R14];
    }

    [[nodiscard]] auto save_stack_trace_at(
        u64 sp, u64 first_addr, trace_entry* buf, u32 max_entries
    ) noexcept -> u32 {
        if (buf == nullptr || max_entries == 0) [[unlikely]]
            return 0;

        u32 depth = 0;

        // A null or unaligned SP cannot begin a valid backchain walk.
        if (sp == 0 || (sp & 0x7) != 0) [[unlikely]]
            return 0;

        u64 cur = sp;
        stack_info info = get_stack_info(cur);
        if (info.type == stack_type::unknown)
            return 0;
        if (!on_stack(cur, info))
            return 0;

        u64 ip = (first_addr != 0) ? first_addr : frame_return_addr(cur);
        if (ip != 0 && depth < max_entries) {
            record_ip(ip, buf, depth);
            ++depth;
        }

        while (depth < max_entries) {
            const auto* sf = reinterpret_cast<const stack_frame*>(cur);
            const u64 back = sf->back_chain;

            if (back == 0 || back <= cur || (back & 0x7) != 0)
                break;

            if (!on_stack(back, info)) {
                const auto next_info = get_stack_info(back);
                if (next_info.type == stack_type::unknown)
                    break;
                info = next_info;
            }

            cur = back;
            const u64 next_ip = frame_return_addr(cur);
            if (next_ip == 0)
                break;

            record_ip(next_ip, buf, depth);
            ++depth;
        }

        return depth;
    }

    [[nodiscard]] auto save_stack_trace(
        trace_entry* buf, u32 max_entries
    ) noexcept -> u32 {
        return save_stack_trace_at(
            cpu::processor::get_current_sp(), 0, buf, max_entries);
    }

    /// @brief Print a backtrace starting from a given SP.
    auto dump_stack_at(u64 sp, u64 first_addr) noexcept -> void {
        using zxfoundation::sys::printk::pinfo;
        using zxfoundation::sys::printk::pnotice;

        // Fixed-size local buffer avoids allocation on the fault path.
        trace_entry entries[MAX_TRACE_DEPTH];
        const u32 count = save_stack_trace_at(sp, first_addr, entries, MAX_TRACE_DEPTH);

        pinfo("Call Trace:");
        for (u32 i = 0; i < count; ++i) {
            const auto& e = entries[i];
            if (e.sym_name != nullptr) {
                pinfo(" [<{:#016x}>] {}+{:#x}", e.addr, e.sym_name, e.addr - e.sym_addr);
            } else {
                pinfo(" [<{:#016x}>] (no symbol)", e.addr);
            }
        }

        if (count == 0) {
            pnotice(" <no backtrace available>");
        }
    }

    /// @brief Print a backtrace from an IRQ frame (interrupt/pgm-check context).
    auto dump_stack_from_irq(
        const arch::s390x::cpu::irq::irq_frame* frame
    ) noexcept -> void {
        using zxfoundation::sys::printk::pinfo;
        using zxfoundation::sys::printk::pnotice;

        if (frame == nullptr) [[unlikely]] {
            pinfo("Call Trace:");
            pnotice(" <no backtrace available>");
            return;
        }

        trace_entry entries[MAX_TRACE_DEPTH];
        u32 depth = 0;

        const u64 interrupted_ip = frame->psw_addr;
        const u64 interrupted_sp = frame->gprs[15];

        if (interrupted_ip != 0 && depth < MAX_TRACE_DEPTH) {
            record_ip_exact(interrupted_ip, entries, depth);
            ++depth;
        }

        depth += save_stack_trace_at(
            interrupted_sp, 0, entries + depth, MAX_TRACE_DEPTH - depth);

        pinfo("Call Trace:");
        for (u32 i = 0; i < depth; ++i) {
            const auto& e = entries[i];
            if (e.sym_name != nullptr) {
                pinfo(" [<{:#016x}>] {}+{:#x}", e.addr, e.sym_name, e.addr - e.sym_addr);
            } else {
                pinfo(" [<{:#016x}>] (no symbol)", e.addr);
            }
        }

        if (depth == 0) {
            pnotice(" <no backtrace available>");
        }
    }

    /// @brief Print a backtrace from the current SP.
    auto dump_stack() noexcept -> void {
        dump_stack_at(cpu::processor::get_current_sp(), 0);
    }

} // namespace arch::s390x::lib::stacktrace