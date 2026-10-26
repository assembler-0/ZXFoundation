/// @file zxfoundation/base/cxxrtse/std.cxx
import lib.error;
import zxfoundation.sys.syschk.core;

extern "C" void abort() noexcept {
    zxfoundation::sys::syschk::syschk_fatal(
        lib::kernel_error::from_generic(lib::generic_error::internal_error),
        "cxxrtse: abort()");
}

namespace std {
    auto terminate() noexcept -> void {
        zxfoundation::sys::syschk::syschk_fatal(lib::kernel_error::from_generic(lib::generic_error::internal_error),
        "cxxrtse: terminate()");
    }
}
