/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sys/irq.zcomponent/desc.cxx
/// @brief Generic IRQ descriptor table

module zxfoundation.sys.irq.desc;
import zxfoundation.sys.irq.consts;
import zxfoundation.scoms.table;

from_global(nomangle) namespace zxfoundation::sys::irq::detail {

    from_global(reference) scoms::scoms_table<irq_desc, IRQ_NR_MAX> g_irq_table;
    from_global(reference) bool g_initialized;

} // namespace zxfoundation::sys::irq::detail

namespace zxfoundation::sys::irq {

    [[nodiscard]] auto irq_get_desc(u16 irq) noexcept -> irq_desc* {
        if (!detail::g_initialized || irq >= IRQ_NR_MAX) return nullptr;
        return detail::g_irq_table.find_raw(irq);
    }

} // namespace zxfoundation::sys::irq