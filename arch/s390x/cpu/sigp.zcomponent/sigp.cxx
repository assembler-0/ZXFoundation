/// SPDX-License-Identifier: Apache 2.0
/// @file sigp.cxxm
/// @brief SIGP (SIGnal Processor) API.

module arch.s390x.cpu.sigp;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.clock;
import arch.s390x.cpu.sync;
import zxfoundation.base.types;
import std;

namespace arch::s390x::cpu::sigp {

    /// @brief Compute the next backoff delay.
    /// @return Delay in TOD units.
    auto sigp_backoff::next_delay() noexcept -> u64 {
        const u64 delay = current_tod;
        if (current_tod < max_tod)
            current_tod = std::min(current_tod * 2, max_tod);
        return delay;
    }

    /// @brief Check whether the deadline has passed.
    [[nodiscard]] auto sigp_backoff::expired() const noexcept -> bool {
        return clock::stck() >= deadline;
    }

    /// @brief Busy-wait for current_tod units.
    auto sigp_backoff::wait() noexcept -> void {
        const u64 delay = next_delay();
        const u64 target = clock::stck() + delay;
        while (clock::stck() < target)
            sync::cpu_relax();
    };

    /// @brief Build a sigp_backoff with default parameters.
    /// @param[in] timeout_tod  Absolute TOD deadline.
    /// @return Initialized backoff state.
    [[nodiscard]] auto default_backoff(u64 timeout_tod) noexcept -> sigp_backoff {
        return sigp_backoff{
            .min_tod     = 4'096ULL,            // ~1 ms
            .max_tod     = 524'288ULL,           // ~128 ms
            .current_tod = 4'096ULL,
            .deadline    = timeout_tod,
        };
    }

    /// @brief Issue a SIGP (Signal Processor) instruction.
    auto issue(u16 addr, u8 order, u64 parm) -> sigp_result {
        processor::register_pair r1{ .even = 0, .odd = parm };
        condition_code cc;
        __asm__ volatile(
            "	sigp	%[r1],%[addr],0(%[order])\n"
            "	ipm	    %[cc]\n"
            "	srl	    %[cc],28\n"
            : [cc] "=d" (cc), [r1] "+d" (r1.pair)
            : [addr] "d" (addr), [order] "a" (order)
            : "cc"
        );
        return { static_cast<u32>(r1.even), cc };
    }

    /// @brief Issue SIGP with exponential backoff until the target CPU
    ///        is no longer busy (CC != 2) or the deadline expires.
    /// @param[in] cpu_addr Target CPU address.
    /// @param[in] order SIGP order code.
    /// @param[in] parm Parameter value.
    /// @param[in] backoff Retry state controlling delay and deadline.
    /// @return condition_code and status pair
    [[nodiscard]] auto issue_bounded(
        u16 cpu_addr,
        u8  order,
        u32 parm,
        sigp_backoff& backoff
    ) noexcept -> sigp_result {
        sigp_result initial_result{};
        do {
            initial_result = issue(cpu_addr, order, parm);
            if (initial_result.cc != SIGP_CC_BUSY)
                return initial_result;
            backoff.wait();
        } while (!backoff.expired());
        return { initial_result.status, SIGP_CC_BUSY };
    }

    /// @brief Issue SIGP with exponential backoff until the target CPU
    ///        is no longer busy (CC != 2) or the deadline expires.
    /// @param[in] cpu_addr Target CPU address.
    /// @param[in] order SIGP order code.
    /// @param[in] parm Parameter value.
    /// @return condition_code and status pair
    [[nodiscard]] auto issue_bounded(
        u16 cpu_addr,
        u8  order,
        u32 parm
    ) noexcept -> sigp_result {
        constexpr u64 TOD_UNITS_PER_MILLISECOND = 4'096'000ULL;
        const u64 deadline = clock::stck() + TOD_UNITS_PER_MILLISECOND * 2'000ULL;
        auto backoff = default_backoff(deadline);
        return issue_bounded(cpu_addr, order, parm, backoff);
    }

    /// @brief Wait until a CPU is stopped, with check-stop recovery.
    /// @param[in] cpu_addr Target CPU address.
    [[nodiscard]] auto wait_cpu_stopped(u16 cpu_addr) noexcept -> sigp_result {
        constexpr u64 TOD_UNITS_PER_MILLISECOND = 4'096'000ULL;
        const u64 deadline = clock::stck() + TOD_UNITS_PER_MILLISECOND * 3'000ULL;
        sigp_result initial_result{};

        {
            auto backoff = default_backoff(deadline);
            do {
                initial_result = issue(cpu_addr, SIGP_SENSE, 0);
                if (initial_result.cc == SIGP_CC_NOT_OPERATIONAL)
                    return initial_result;
                if (initial_result.cc == SIGP_CC_BUSY) {
                    backoff.wait();
                    continue;
                }
                if (initial_result.status & (SIGP_STATUS_STOPPED | SIGP_STATUS_CHECK_STOP))
                    return { initial_result.status, SIGP_CC_ORDER_CODE_ACCEPTED };
                backoff.wait();
            } while (!backoff.expired());
        }

        if (initial_result.status & SIGP_STATUS_CHECK_STOP) {
            if (auto reset_result = issue(cpu_addr, SIGP_INITIAL_CPU_RESET, 0);
                reset_result.cc != SIGP_CC_ORDER_CODE_ACCEPTED)
                return reset_result;

            // Retry SENSE after reset (short timeout).
            const u64 reset_deadline = clock::stck() + TOD_UNITS_PER_MILLISECOND * 500ULL;
            auto reset_backoff = default_backoff(reset_deadline);
            do {
                auto sense_result = issue(cpu_addr, SIGP_SENSE, 0);
                if (sense_result.cc == SIGP_CC_NOT_OPERATIONAL)
                    return sense_result;
                if (sense_result.cc != SIGP_CC_BUSY && (sense_result.status & SIGP_STATUS_STOPPED))
                    return { sense_result.status, SIGP_CC_ORDER_CODE_ACCEPTED };
                reset_backoff.wait();
            } while (!reset_backoff.expired());
        }

        return { initial_result.status, SIGP_CC_BUSY };
    }

} // namespace arch::s390x::cpu::sigp