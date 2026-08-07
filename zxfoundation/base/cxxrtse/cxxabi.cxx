/// SPDX-License-Identifier: Apache-2.0
/// @file cxxabi.cxxm
/// @brief minimal cxxabi implementation

import zxfoundation.base.types;
import zxfoundation.sys.syschk;
import lib.error;

namespace std {
    auto terminate() noexcept -> void {
        zxfoundation::sys::syschk::syschk_fatal(lib::kernel_error::from_generic(lib::generic_error::internal_error),
        "cxxrtse: terminate()");
    }
}