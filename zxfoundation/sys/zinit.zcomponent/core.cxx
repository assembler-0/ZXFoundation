/// SPDX-License-Identifier: Apache-2.0
/// @file zinit.cxxm
/// @brief initcall system

module zxfoundation.sys.zinit.core;
import zxfoundation.base.types;
import zxfoundation.sys.syschk;
import zxfoundation.sys.printk.core;
import lib.error;
import std;

from_global(nomangle) {
    from_global(reference) const zxfoundation::sys::zinit::zi_desc __zinit_start[];
    from_global(reference) const zxfoundation::sys::zinit::zi_desc __zinit_end[];
}

namespace zxfoundation::sys::zinit::detail {

    [[nodiscard]] no_export auto execute(const zi_desc* desc) noexcept
        -> std::expected<void, lib::kernel_error> {
        using namespace zxfoundation::sys;
        if (!desc || !desc->init) return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        if (!(desc->action & zinit::zi_action::silent))
            printk::pinfo("zinit: running entry {}", desc->name);
        const auto d { *desc }; // TODO: stop copying
        if (auto ret = desc->init(d); !ret) {
            if (desc->action & zinit::zi_action::warn)
                syschk::syschk_warning(ret.error(), "zinit: entry {} failed", desc->name);
            else if (desc->action & zinit::zi_action::fatal)
                syschk::syschk_fatal(ret.error(), "zinit: entry {} failed", desc->name);
            else if (!(desc->action & zinit::zi_action::silent))
                printk::pnotice("zinit: entry {} failed", desc->name);
            return ret;
        }
        if (!(desc->action & zinit::zi_action::silent))
            printk::pinfo("zinit: entry {} initialized", desc->name);
        return {};
    }

}

namespace zxfoundation::sys::zinit {

    [[nodiscard]] auto run_class(const zi_class cls) noexcept -> u32 {
        u32 fail_count = 0;
        for (auto* desc = __zinit_start; desc < __zinit_end; ++desc) {
            if (desc->cls == cls) {
                if (!detail::execute(desc)) ++fail_count;
            }
        }
        return fail_count;
    }

} // namespace zxfoundation::sys::zinit
