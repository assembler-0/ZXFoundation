/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/printk.zcomponent/core.cxx
/// @brief Kernel logging framework.

module zxfoundation.sys.printk.core;
import zxfoundation.base.types;
import zxfoundation.base.config;
import zxfoundation.sys.printk.types;
import zxfoundation.sys.time.core;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sync.lockable;
import zxfoundation.dgp.domain.types;
import zxfoundation.sys.zinit.core;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.lowcore;
import lib.format;
import lib.ring_buffer;
import lib.static_vector;
import lib.stl;
import :state;

#define __zxmacro_want_zinit
#include macro_extensions
import lib.error;

namespace zxfoundation::sys::printk {

    auto set_min_level(const log_level level) noexcept -> void {
        g_state.min_level = level;
    }

    auto get_current_level() noexcept -> log_level {
        return g_state.min_level;
    }

    auto handle_min_level(std::string_view val) noexcept -> bool {
        if (val.empty()) [[unlikely]]
            return false;
        u64 level = 0;
        for (char c : val) {
            if (c < '0' || c > '9') [[unlikely]]
                return false;
            level = level * 10 + (c - '0');
        }
        if (level > 7) [[unlikely]]
            return false;
        set_min_level(static_cast<log_level>(level));
        return true;
    }

    auto zi_printk_init(const zinit::zi_desc&) noexcept -> zinit::zi_result {
        return {};
    }
    __zinit__(early, p20, zi_printk_init, fatal)

    auto register_backend(const printk_backend& backend) noexcept -> std::expected<void, lib::kernel_error> {
        if (!backend.emit) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::uninitialized));
        }
        const auto irq_flags = arch::s390x::cpu::irq::irq_save_disable();
        g_state.lock.lock();
        if (g_state.backends.full()) {
            g_state.lock.unlock();
            arch::s390x::cpu::irq::irq_restore(irq_flags);
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::out_of_range));
        }
        if (backend.init) backend.init();
        auto b = backend;
        b.active = true;
        discard_value g_state.backends.push_back(b);
        g_state.lock.unlock();
        arch::s390x::cpu::irq::irq_restore(irq_flags);
        return {};
    }

    auto read_logs(std::span<u8> dest, bool consume) noexcept -> usize {
        const auto irq_flags = arch::s390x::cpu::irq::irq_save_disable();
        g_state.lock.lock();
        usize result;
        if (consume) {
            result = g_state.log_buffer.pop(dest);
        } else {
            result = g_state.log_buffer.peek(dest);
        }
        g_state.lock.unlock();
        arch::s390x::cpu::irq::irq_restore(irq_flags);
        return result;
    }

    auto log_size() noexcept -> usize {
        const auto irq_flags = arch::s390x::cpu::irq::irq_save_disable();
        g_state.lock.lock();
        const usize result = g_state.log_buffer.size();
        g_state.lock.unlock();
        arch::s390x::cpu::irq::irq_restore(irq_flags);
        return result;
    }

    /// @brief Commit a pre-formatted message to all active backends and the ring-buffer.
    /// @param[in] buf     Pre-formatted message buffer (NUL-terminated).
    /// @param[in] offset  Number of valid bytes in buf (not counting NUL).
    /// @param[in] cpu     Logical CPU ID of the calling processor.
    auto printk_commit(const char* buf, usize offset) noexcept -> void {
        const auto irq_flags = arch::s390x::cpu::irq::irq_save_disable();

        bool pushed = false;
        if (g_state.lock.try_lock()) {
            std::span<const u8> to_push(
                reinterpret_cast<const u8*>(buf), offset);
            while (to_push.size() > 0) {
                usize written = g_state.log_buffer.push(to_push);
                if (written == to_push.size()) break;
                u8 discard[1024];
                discard_value g_state.log_buffer.pop(
                    {discard, std::min(static_cast<usize>(sizeof(discard)),
                                       g_state.log_buffer.size())});
                if (written > 0) to_push = to_push.subspan(written);
            }
            g_state.lock.unlock();
            pushed = true;
        }

        if (pushed) {
            if (g_state.flush_lock.try_lock()) {
                bool done = false;
                while (!done) {
                    while (true) {
                        lib::static_vector<printk_backend, MAX_BACKENDS> snap;
                        char line_buf[PRINTK_LINE_MAX];
                        usize popped = 0;

                        g_state.lock.lock();
                        for (auto & backend : g_state.backends) {
                            if (backend.active) {
                                discard_value snap.push_back(backend);
                            }
                        }

                        const usize peeked = g_state.log_buffer.peek(
                            {reinterpret_cast<u8*>(line_buf), PRINTK_LINE_MAX});
                        
                        usize line_len = 0;
                        for (usize i = 0; i < peeked; ++i) {
                            if (line_buf[i] == '\n') {
                                line_len = i + 1;
                                break;
                            }
                        }
                        if (line_len == 0) {
                            line_len = peeked;
                        }

                        if (line_len > 0) {
                            popped = g_state.log_buffer.pop(
                                {reinterpret_cast<u8*>(line_buf), line_len});
                        }
                        g_state.lock.unlock();

                        if (popped == 0) {
                            break; // No more log messages
                        }

                        const std::string_view msg(line_buf, popped);
                        for (auto & i : snap) {
                            i.emit(msg);
                        }
                    }

                    g_state.flush_lock.unlock();

                    g_state.lock.lock();
                    const bool more_logs = (g_state.log_buffer.size() > 0);
                    g_state.lock.unlock();

                    if (more_logs) {
                        if (!g_state.flush_lock.try_lock()) {
                            done = true;
                        }
                    } else {
                        done = true;
                    }
                }
            }
        } else {
            lib::static_vector<printk_backend, MAX_BACKENDS> snap;
            for (auto & backend : g_state.backends) {
                if (backend.active) {
                    discard_value snap.push_back(backend);
                }
            }
            const std::string_view msg(buf, offset);
            for (auto & i : snap) {
                i.emit(msg);
            }
        }

        arch::s390x::cpu::irq::irq_restore(irq_flags);
    }

} // namespace zxfoundation::sys::printk
