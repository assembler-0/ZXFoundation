/// SPDX-License-Identifier: Apache-2.0
/// @file cxxabi.cxxm
/// @brief minimal cxxabi implementation

import zxfoundation.base.types;
import zxfoundation.sys.syschk.core;
import lib.error;
import lib.stl;

namespace std {
    auto terminate() noexcept -> void {
        zxfoundation::sys::syschk::syschk_fatal(lib::kernel_error::from_generic(lib::generic_error::internal_error),
        "cxxrtse: terminate()");
    }
}

#if __cpp_contracts >= 202400L
#if __has_include(<contracts>)

auto handle_contract_violation(std::contracts::contract_violation const& cv) -> void {
    zxfoundation::sys::syschk::syschk_fatal(
        lib::kernel_error::from_generic(lib::generic_error::internal_error),
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