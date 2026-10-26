/// @file zxfoundation/sys/irq.zcomponent/storage.cxx
import zxfoundation.scoms.table;
import zxfoundation.sys.irq.desc;
import zxfoundation.sys.irq.consts;

declscope(C) namespace zxfoundation::sys::irq::detail {

    scoms::scoms_table<irq_desc, IRQ_NR_MAX> g_irq_table{};
    bool g_initialized{false};

} // namespace zxfoundation::sys::irq::detail