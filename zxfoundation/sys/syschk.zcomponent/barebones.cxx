/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/syschk.zcomponent/barebones.cxx
/// @brief Barebones mode

module zxfoundation.sys.syschk.barebones;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.syschk.types;
import lib.error;
import lib.stl;

namespace zxfoundation::sys::syschk {

    [[gnu::noinline]] [[noreturn]] auto bug(const panic_info& info) noexcept -> void {
        system_check(lib::kernel_error::from_generic(lib::generic_error::internal_error),
            syschk_severity::fatal,
            info.msg,
            info.loc
        );
        __builtin_unreachable();
    }

} // namespace zxfoundation::sys::syschk
