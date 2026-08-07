/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/processor.cxxm
/// @brief cpu synchronization

module arch.s390x.cpu.sync;
import arch.s390x.cpu.diag;
import std;

namespace arch::s390x::cpu::sync {

    /// @brief Spin-loop hint.
    auto cpu_relax() noexcept -> void {
        diag::diag44();
    }

    /// @brief Yield the virtual CPU timeslice to the hypervisor (if running under one).
    auto cpu_yield() noexcept -> void {
        cpu_relax();
    }

    /// @brief Serialization Sync (CPU serialization).
    auto ssync() noexcept -> void {
        __asm__ volatile("bcr 15,0" ::: "memory");
    }

    /// @brief Memory Sync
    auto msync() noexcept -> void {
        std::atomic_thread_fence(std::memory_order_seq_cst);;
    }

    /// @brief compiler barrier
    auto barrier() noexcept -> void {
        __asm__ volatile("" ::: "memory");
    }

} // namespace arch::s390x::cpu::processor
