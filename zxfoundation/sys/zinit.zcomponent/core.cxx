/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/zinit.zcomponent/core.cxx
/// @brief zinit engine — initcall core.

module zxfoundation.sys.zinit.core;
import zxfoundation.base.types;
import zxfoundation.sys.syschk;
import zxfoundation.sys.printk.core;
import lib.error;
import lib.stl;

extern "C" {
    extern const zxfoundation::sys::zinit::zi_desc __zinit_start[];
    extern const zxfoundation::sys::zinit::zi_desc __zinit_end[];
}

namespace zxfoundation::sys::zinit {

    [[nodiscard]] auto execute(const zi_desc& desc) noexcept
        -> std::expected<void, lib::kernel_error> {
        if (!desc.init)
            return std::unexpected(
                lib::kernel_error::from_generic(lib::generic_error::invalid_arg));

        if (auto ret = desc.init(desc); !ret) {
            if (desc.action & zi_action::warn)
                syschk::syschk_warning(ret.error(),
                                       "zinit: entry {} failed", desc.name);
            else if (desc.action & zi_action::fatal)
                syschk::syschk_fatal(ret.error(),
                                     "zinit: entry {} failed", desc.name);
            return ret;
        }

        return {};
    }

    [[nodiscard]] auto run_class(const zi_class cls) noexcept -> u32 {
        u32 fail_count = 0;

        for (u32 tier = 0; tier <= 255; ++tier) {
            const auto tier_prio = static_cast<zi_prio>(tier);
            for (const zi_desc* d = __zinit_start; d < __zinit_end; ++d) {
                if (d->cls == cls && d->prio == tier_prio) {
                    if (!execute(*d)) ++fail_count;
                }
            }
        }
        return fail_count;
    }

    [[nodiscard]] auto run_all() noexcept -> u32 {
        u32 total_failed = 0;
        for (u8 c = 0; c < static_cast<u8>(zi_class::max); ++c) {
            total_failed += run_class(static_cast<zi_class>(c));
        }
        return total_failed;
    }

    [[nodiscard]] auto count() noexcept -> usize {
        return static_cast<usize>(__zinit_end - __zinit_start);
    }

    [[nodiscard]] auto descriptors() noexcept -> std::span<const zi_desc> {
        return std::span<const zi_desc>(__zinit_start, __zinit_end);
    }

} // namespace zxfoundation::sys::zinit
