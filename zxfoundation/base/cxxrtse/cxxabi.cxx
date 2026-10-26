/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/base/cxxrtse/cxxabi.cxx
/// @brief C++ ABI runtime implementation for z/Architecture (s390x)

module zxfoundation.base.cxxrtse.cxxabi;
import zxfoundation.base.types;
import zxfoundation.sys.syschk.core;
import zxfoundation.sync.lockable;
import zxfoundation.sync.qspinlock.core;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.zinit.core;
import arch.s390x.cpu.sync;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.clock;
import lib.static_vector;
import lib.error;
import lib.stl;

namespace detail {

    constexpr usize CXA_ATEXIT_MAX = 512;

    struct AtExitEntry {
        void (*destructor)(void*) noexcept;
        void* obj;
        void* dso;
    };

    lib::static_vector<AtExitEntry, CXA_ATEXIT_MAX> g_atexit_table{};
    alignas(4) zxfoundation::sync::qspinlock::qspinlock<zxfoundation::sync::lock_rank::cxxrt> g_atexit_lock{};

    /// @brief Avalanche bit mixer for hardware clock entropy (SplitMix64).
    [[nodiscard]] constexpr auto splitmix64(u64 x) noexcept -> u64 {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

} // namespace detail

declscope(C) {

    from_global(reference) void (*__init_array_start[])() __attribute__((weak));
    from_global(reference) void (*__init_array_end[])()   __attribute__((weak));
    from_global(reference) void (*__fini_array_start[])() __attribute__((weak));
    from_global(reference) void (*__fini_array_end[])()   __attribute__((weak));
    from_global(reference) void *__dso_handle __attribute__((weak));

    inline constexpr u64 GUARD_INITIALIZED_BIT = 0x0100'0000'0000'0000ULL;
    inline constexpr u64 GUARD_IN_PROGRESS_BIT  = 0x0001'0000'0000'0000ULL;
    inline constexpr u64 GUARD_CPU_TOKEN_MASK   = 0x0000'0000'0000'FFFFULL;

    auto __cxa_guard_acquire(u64* g) noexcept -> int {
        auto* guard = reinterpret_cast<std::atomic<u64>*>(g);
        const u64 cpu_token = (static_cast<u64>(arch::s390x::cpu::processor::cpu_addr()) + 1ULL) & GUARD_CPU_TOKEN_MASK;

        while (true) {
            u64 val = guard->load(std::memory_order_acquire);

            // Fast path: static variable already constructed
            if (val & GUARD_INITIALIZED_BIT) [[likely]] {
                return 0;
            }

            // Uninitialized & unclaimed: attempt acquisition
            if ((val & (GUARD_INITIALIZED_BIT | GUARD_IN_PROGRESS_BIT)) == 0) {
                const u64 desired = GUARD_IN_PROGRESS_BIT | cpu_token;
                if (guard->compare_exchange_weak(val, desired,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {
                    return 1; // We won the race; caller executes constructor
                }
                continue;
            }

            // Detect recursive/circular initialization on the same CPU core
            if ((val & GUARD_IN_PROGRESS_BIT) && ((val & GUARD_CPU_TOKEN_MASK) == cpu_token)) [[unlikely]] {
                zxfoundation::sys::syschk::syschk_fatal(
                    lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
                    "cxxabi: recursive static initialization detected on CPU core."
                );
            }

            // Guard is held by another CPU: yield / pause pipeline
            arch::s390x::cpu::sync::cpu_relax();
        }
    }

    auto __cxa_guard_release(u64* g) noexcept -> void {
        auto* guard = reinterpret_cast<std::atomic<u64>*>(g);
        guard->store(GUARD_INITIALIZED_BIT, std::memory_order_release);
    }

    auto __cxa_guard_abort(u64* g) noexcept -> void {
        auto* guard = reinterpret_cast<std::atomic<u64>*>(g);
        guard->store(0, std::memory_order_release);
    }

    auto __cxa_atexit(void (*destructor)(void*), void* obj, void* dso) noexcept -> int {
        using namespace detail;
        if (!destructor) [[unlikely]] {
            return 0;
        }

        bool ok = false;
        {
            zxfoundation::sync::lock_guard _(g_atexit_lock);
            ok = g_atexit_table.push_back({
                reinterpret_cast<void (*)(void*) noexcept>(destructor),
                obj,
                dso
            }).has_value();
        }

        if (!ok) [[unlikely]] {
            zxfoundation::sys::syschk::syschk_fatal(
                lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
                "cxxrtse: __cxa_atexit: static atexit table exhausted."
            );
        }
        return 0;
    }

    auto __cxa_finalize(void* dso) noexcept -> void {
        using namespace detail;

        while (true) {
            AtExitEntry entry{};

            {
                zxfoundation::sync::lock_guard _(g_atexit_lock);

                // Reclaim dead entries from the tail
                while (!g_atexit_table.empty() && g_atexit_table.back().destructor == nullptr) {
                    g_atexit_table.pop_back();
                }

                if (g_atexit_table.empty()) {
                    break;
                }

                // Search backwards for the most recent un-invoked destructor (LIFO)
                usize idx = g_atexit_table.size();
                bool found = false;

                while (idx > 0) {
                    --idx;
                    auto& candidate = g_atexit_table[idx];
                    if (candidate.destructor != nullptr && (!dso || candidate.dso == dso)) {
                        entry = candidate;
                        candidate.destructor = nullptr; // Mark as dispatched
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    break;
                }
            }

            if (entry.destructor != nullptr) {
                entry.destructor(entry.obj);
            }
        }
    }

    [[noreturn]] auto __cxa_pure_virtual() -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_pure_virtual(): call to pure virtual function."
        );
    }

    [[noreturn]] auto __cxa_deleted_virtual() -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_deleted_virtual(): call to deleted virtual function."
        );
    }

    [[noreturn]] auto __cxa_bad_typeid() -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_bad_typeid(): typeid applied to null pointer."
        );
    }

    [[noreturn]] auto __cxa_bad_cast() -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_bad_cast(): dynamic_cast to reference failed."
        );
    }

    [[noreturn]] auto __cxa_throw(void*, void*, void (*)(void*)) -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_throw(): exception thrown in no-exception kernel context."
        );
    }

    [[noreturn]] auto __cxa_rethrow() -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_rethrow(): exception rethrow attempted in kernel context."
        );
    }

    auto __cxa_begin_catch(void*) noexcept -> void* {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_begin_catch(): catch block reached in kernel context."
        );
    }

    auto __cxa_end_catch() noexcept -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_end_catch(): end-catch block reached in kernel context."
        );
    }

    auto __cxa_allocate_exception(u64) noexcept -> void* {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_allocate_exception(): exception allocation requested."
        );
    }

    auto __cxa_free_exception(void*) noexcept -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_free_exception(): exception deallocation requested."
        );
    }

    [[noreturn]] auto __cxa_call_unexpected(void*) -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_call_unexpected(): unexpected exception specification breached."
        );
    }

    [[noreturn]] auto __cxa_call_terminate(void*) noexcept -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
            "cxxabi: __cxa_call_terminate(): terminate handler invoked."
        );
    }

    u64 __stack_chk_guard = 0x595A465353500001ULL;

    [[gnu::no_stack_protector, noreturn]] auto __stack_chk_fail() -> void {
        zxfoundation::sys::syschk::syschk_fatal(
            lib::kernel_error::from_memory(lib::memory_error::ssp_check_fail),
            "cxxrtse: kernel stack smashing detected (corrupted return address)."
        );
        __builtin_unreachable();
    }

} // declscope(C)

namespace zxfoundation::base::cxxrtse::cxxabi {

    /// @brief Install kernel-wide stack canary entropy prior to any complex calls.
    [[gnu::no_stack_protector]] auto ssp_init() noexcept -> void {
        const u64 tod = arch::s390x::cpu::clock::stck();
        u64 canary = detail::splitmix64(tod ^ 0xC0FFEE00FACADE42ULL);
        canary &= 0x00FF'FFFF'FFFF'FFFFULL;
        __stack_chk_guard = canary;
    }

    auto run_global_constructor() noexcept -> void {
        if (!&__init_array_start || !&__init_array_end) [[unlikely]] {
            return;
        }

        const auto* start = __init_array_start;
        const auto* end   = __init_array_end;

        if (start >= end) [[unlikely]] {
            return;
        }

        const auto count = static_cast<usize>(end - start);
        for (usize i = 0; i < count; ++i) {
            void (*fn)() = start[i];
            if (fn != nullptr) [[likely]] {
                fn();
            }
        }
    }

    /// @brief Primary runtime initialization entry point.
    auto init() noexcept -> void {
        ssp_init();
        run_global_constructor();
    }

    [[nodiscard]] auto stack_guard() noexcept -> u64 {
        return __stack_chk_guard;
    }

    void run_fini_array() noexcept {
        if (!&__fini_array_start || !&__fini_array_end) [[unlikely]] {
            sys::printk::pinfo("cxxabi: __fini_array symbols missing. Skipping.");
            return;
        }

        const auto* start = __fini_array_start;
        const auto* end   = __fini_array_end;

        if (start >= end) [[unlikely]] {
            return;
        }

        const auto count = static_cast<usize>(end - start);
        sys::printk::pinfo("cxxabi: running {} fini_array entries (reverse order).", count);

        for (usize i = count; i > 0; --i) {
            void (*fn)() = start[i - 1];
            if (fn != nullptr) [[likely]] {
                fn();
            }
        }

        sys::printk::pinfo("cxxabi: all fini_array entries completed.");
    }

    void run_global_destructors() noexcept {
        __cxa_finalize(nullptr);
        run_fini_array();
    }

} // namespace zxfoundation::base::cxxrtse::cxxabi

#if __cpp_contracts >= 202400L
#if __has_include(<contracts>)

auto handle_contract_violation(std::contracts::contract_violation const& cv) -> void {
    zxfoundation::sys::syschk::syschk_fatal(
        lib::kernel_error::from_generic(lib::generic_error::code::internal_error),
        "cxxrtse: contract violation: {} [{}/{}/{}] at {} (is terminating: {})",
        cv.comment(),
        static_cast<u16>(cv.kind()),
        static_cast<u16>(cv.semantic()),
        static_cast<u16>(cv.mode()),
        cv.location(),
        cv.is_terminating()
    );
}

#endif
#endif