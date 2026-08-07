/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/cpumask.cxxm
/// @brief Canonical possible / online CPU registry for the s390x platform.

module arch.s390x.cpu.cpumask;
import zxfoundation.base.types;
import std;
import :storage;

namespace arch::s390x::cpu::cpumask {

    using memory_order = std::memory_order;

    /// @brief Mark a CPU as possible (physically present).
    /// @param[in] id CPU identifier from the boot cpu_map.
    /// @note Called only during single-threaded boot setup.
    auto cpu_set_possible(u16 id) noexcept -> void {
        _internal::g_possible_mask.set(id, memory_order::relaxed);
    }

    /// @brief Test whether a CPU is possible (physically present).
    /// @param[in] id CPU identifier.
    [[nodiscard]] auto cpu_possible(u16 id) noexcept -> bool {
        return _internal::g_possible_mask.test(id, memory_order::relaxed);
    }

    /// @brief Publish a CPU as online (fully initialized, safe to target).
    /// @param[in] id CPU identifier.
    /// @note Uses release ordering so that all per-CPU initialization performed
    ///       before this call is visible to any observer that subsequently sees
    ///       the bit via for_each_online_cpu (acquire).
    auto cpu_set_online(u16 id) noexcept -> void {
        _internal::g_online_mask.set(id, memory_order::release);
    }

    /// @brief Remove a CPU from the online set (halt / teardown).
    /// @param[in] id CPU identifier.
    /// @note Release ordering; once cleared the slot must not be revisited.
    auto cpu_clear_online(u16 id) noexcept -> void {
        _internal::g_online_mask.clear(id, memory_order::release);
    }

    /// @brief Test whether a CPU is currently online.
    /// @param[in] id CPU identifier.
    [[nodiscard]] auto cpu_online(u16 id) noexcept -> bool {
        return _internal::g_online_mask.test(id, memory_order::acquire);
    }

    /// @brief Number of CPUs currently online.
    [[nodiscard]] auto num_online_cpus() noexcept -> u32 {
        return static_cast<u32>(_internal::g_online_mask.count());
    }

    /// @brief Number of CPUs that physically exist.
    [[nodiscard]] auto num_possible_cpus() noexcept -> u32 {
        return static_cast<u32>(_internal::g_possible_mask.count());
    }

} // namespace arch::s390x::cpu::cpumask
