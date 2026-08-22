/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/cpu/uaccess.cxxm
/// @brief z/Architecture user-space access

module arch.s390x.cpu.uaccess;
import arch.s390x.cpu.features;
import arch.s390x.mmu.consts;
import arch.s390x.mmu.core;
import arch.s390x.mmu.types;
import zxfoundation.base.types;
import zxfoundation.memory.hhdm;
import std;

namespace arch::s390x::cpu::uaccess::detail {

    /// OAC: use PSW ASC (= HOME for kernel).  K=0,A=0 ⇒ 0x0000.
    no_export constexpr u32 OAC_HOME    = 0x0000U;

    /// OAC: explicit primary space.  A=1, AS=00 ⇒ 0x0001.
    no_export constexpr u32 OAC_PRIMARY = 0x0001U;

    /// GR0 for copy_from_user: dest=HOME, src=PRIMARY.
    no_export constexpr u64 GR0_FROM_USER =
        (static_cast<u64>(OAC_HOME) << 16) | OAC_PRIMARY;   // 0x0000'0001

    /// GR0 for copy_to_user: dest=PRIMARY, src=HOME.
    no_export constexpr u64 GR0_TO_USER =
        (static_cast<u64>(OAC_PRIMARY) << 16) | OAC_HOME;   // 0x0001'0000

    /// @brief Fallback copy from user space via manual page-table walk.
    [[nodiscard]]
    no_export auto dat_copy_from_user(void* dst, u64 src, u64 len,
                                          u64 root_phys) noexcept -> i32 {
        namespace hhdm = zxfoundation::memory::hhdm;

        auto* out = static_cast<u8*>(dst);
        const mmu::dat_table<mmu::dat_level::region_1> root{root_phys};

        u64 copied = 0;
        while (copied < len) {
            const u64 user_va  = src + copied;
            const u64 page_off = user_va & 0xFFFULL;
            const u64 chunk    = std::min(len - copied,
                                            mmu::PAGE_SIZE_4K - page_off);

            auto phys_r = mmu::walk<mmu::dat_level::region_1>(root, user_va);
            if (!phys_r) [[unlikely]]
                return -1;

            const auto* page_ptr = reinterpret_cast<const u8*>(
                hhdm::phys_to_virt(*phys_r));

            std::copy_n(page_ptr, chunk, out + copied);
            copied += chunk;
        }
        return 0;
    }

    /// @brief Fallback copy to user space via manual page-table walk.
    [[nodiscard]]
    no_export auto dat_copy_to_user(u64 dst, const void* src, u64 len,
                                        u64 root_phys) noexcept -> i32 {
        namespace hhdm = zxfoundation::memory::hhdm;

        const auto* in = static_cast<const u8*>(src);
        const mmu::dat_table<mmu::dat_level::region_1> root{root_phys};

        u64 copied = 0;
        while (copied < len) {
            const u64 user_va  = dst + copied;
            const u64 page_off = user_va & 0xFFFULL;
            const u64 chunk    = ::std::min(len - copied,
                                            mmu::PAGE_SIZE_4K - page_off);

            auto phys_r = mmu::walk<mmu::dat_level::region_1>(root, user_va);
            if (!phys_r) [[unlikely]]
                return -1;

            auto* page_ptr = reinterpret_cast<u8*>(
                hhdm::phys_to_virt(*phys_r));

            ::std::copy_n(in + copied, chunk, page_ptr);
            copied += chunk;
        }
        return 0;
    }

} // namespace arch::s390x::cpu::uaccess::detail

namespace arch::s390x::cpu::uaccess {

    using namespace detail;

    /// @brief Copy from user space (PRIMARY) to kernel space (HOME).
    [[nodiscard]] auto copy_from_user(void* dst, u64 src, u64 len,
                                             u64 root_phys) noexcept -> i32 {
        if (len == 0) [[unlikely]] return 0;
        if (features::has(features::facility::mvcos)) {
            return mvcos_copy<EX_TYPE_UA_MVCOS_FROM>(dst,
                reinterpret_cast<const void*>(src),
                len, GR0_FROM_USER);
        }
        return dat_copy_from_user(dst, src, len, root_phys);
    }

    /// @brief Copy from kernel space (HOME) to user space (PRIMARY).
    [[nodiscard]] auto copy_to_user(u64 dst, const void* src, u64 len,
                                           u64 root_phys) noexcept -> i32 {
        if (len == 0) [[unlikely]]
            return 0;
        if (features::has(features::facility::mvcos)) {
            return mvcos_copy<EX_TYPE_UA_MVCOS_TO>(reinterpret_cast<void*>(dst), src, len, GR0_TO_USER);
        }
        return dat_copy_to_user(dst, src, len, root_phys);
    }

} // namespace arch::s390x::cpu::uaccess
