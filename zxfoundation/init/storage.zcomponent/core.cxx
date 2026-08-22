/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/init/bootstrap.cxxm
/// @brief sys/bootstrapd — first schedulable kernel domain.

module zxfoundation.init.storage.core;
import arch.s390x.init.zxfl.protocol;

namespace zxfoundation::init::storage::detail {

    no_export arch::s390x::init::zxfl::protocol kernel_protocol{};

}

namespace zxfoundation::init::storage {

    /// @brief Store kernel boot protocol
    auto set_protocol(const arch::s390x::init::zxfl::protocol& proto) noexcept -> void {
        detail::kernel_protocol = proto;
    }

    /// @brief Get kernel boot protocol
    auto get_protocol() noexcept -> const arch::s390x::init::zxfl::protocol& {
        return detail::kernel_protocol;
    }

} // namespace zxfoundation::init::bootstrap
