/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/io/ccw.zcomponent/io.cxx
/// @brief CCW I/O engine — synchronous I/O, SENSE ID, and device probing.

module arch.s390x.io.ccw.core;
import arch.s390x.io.css.core;
import arch.s390x.io.css.subchannel;
import arch.s390x.io.types;
import arch.s390x.io.devmatch;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.sync;
import arch.s390x.cpu.irq;
import arch.s390x.cpu.clock;
import zxfoundation.base.types;
import zxfoundation.sys.printk.core;
import zxfoundation.memory.pmm;
import zxfoundation.memory.pmm.types;
import zxfoundation.memory.hhdm;
import lib.error;
import lib.format;
import lib.structure;
import lib.bitfield;
import lib.stl;

namespace arch::s390x::io::ccw {

    /// @brief Execute a CCW chain synchronously (polling mode).
    /// @param sch_index  Index into the subchannel table.
    /// @param channel_program_addr Pointer to CCW chain in DMA31-addressable memory.
    /// @param key        Storage key for the ORB (default 0 = nucleus).
    /// @param timeout_us Timeout in microseconds (default 5 s).
    /// @return The IRB captured by the interrupt handler on success,
    ///         kernel_error on timeout or I/O failure.
    [[nodiscard]] auto sync(
        u32 sch_index,
        u32 channel_program_addr,
        u8 key,
        u64 timeout_us
    ) noexcept -> ::std::expected<irb, ::lib::kernel_error> {
        using namespace zxfoundation::sys::printk;
        using namespace zxfoundation::memory::hhdm;

        // Validate the subchannel entry.
        auto* entry = css::get_entry(sch_index);
        if (!entry) {
            return ::std::unexpected(
                ::lib::kernel_error::from_generic(::lib::generic_error::out_of_range));
        }

        // Drain any stale status left from a prior operation before arming.
        {
            irb discard{};
            discard_value css::tsch(entry->schid, discard);
        }

        entry->io_done.store(0, std::memory_order::seq_cst);
        orb o{
            "subchannel_key"_f = key,
            "channel_program_address"_f = channel_program_addr,
            "logical_path_mask"_f = lib::bf8{0xff},
            R"(control\ccw_format)"_f = 1
        };

        pverbose("ccw_io: SSCH sch={} schid={} cpa={} ctrl={}",
              ::lib::format::dec{static_cast<u64>(sch_index)},
              ::lib::format::hex{static_cast<u64>(entry->schid.underlying()), 8, true},
              ::lib::format::hex{static_cast<u64>(o.get<"channel_program_address">()), 8, true},
              ::lib::format::hex{static_cast<u64>(o.get<"control">().underlying()), 8, true});

        constexpr u32 MAX_START_RETRIES = 3;
        for (u32 retry = 0; retry < MAX_START_RETRIES; ++retry) {
            const int cc = css::ssch(entry->schid, o);
            if (cc == 0) {
                entry->state = css::subchannel_state::active;
                break;
            }

            // Log the raw CC and subchannel status for diagnostics.
            schib schib_diag{};
            const int stsch_cc = css::stsch(entry->schid, schib_diag);

            pwarn("ccw_io: SSCH cc={} retry={} stsch_cc={} pmcw_flags={} dev_st={} sch_st={}",
                  ::lib::format::dec{static_cast<u64>(cc)},
                  ::lib::format::dec{static_cast<u64>(retry)},
                  ::lib::format::dec{static_cast<u64>(stsch_cc)},
                  ::lib::format::hex{static_cast<u64>(schib_diag.get<R"(pmcw\flags)">().underlying()), 4, true},
                  ::lib::format::hex{static_cast<u64>(schib_diag.get<R"(scsw\dev_status)">().underlying()), 2, true},
                  ::lib::format::hex{static_cast<u64>(schib_diag.get<R"(scsw\sch_status)">().underlying()), 2, true});

            if (retry + 1 == MAX_START_RETRIES) {
                return ::std::unexpected(
                    ::lib::kernel_error::from_driver(::lib::driver_error::io_error));
            }

            // Drain whatever status is pending and retry.
            irb discard{};
            discard_value css::tsch(entry->schid, discard);
        }

        // Calculate timeout deadline in TOD ticks (1 µs = 4096 ticks).
        const u64 start_tod = cpu::clock::stck();
        const u64 deadline  = start_tod + (timeout_us * 4096ULL);

        while (true) {
            if (entry->io_done.load(std::memory_order::seq_cst) != 0) {
                irb result_irb{};
                ::std::copy_n(reinterpret_cast<const u8*>(&entry->last_irb), sizeof(irb), result_irb.raw_ptr());

                if (result_irb.get<"scsw">().has_device_error() || result_irb.get<"scsw">().has_sch_error()) {
                    pwarn("ccw_io: I/O error sch={} dev_status={} sch_status={} residual={}",
                          ::lib::format::dec{static_cast<u64>(sch_index)},
                          ::lib::format::hex{static_cast<u64>(result_irb.get<R"(scsw\dev_status)">().underlying()), 2, true},
                          ::lib::format::hex{static_cast<u64>(result_irb.get<R"(scsw\sch_status)">().underlying()), 2, true},
                          ::lib::format::dec{static_cast<u64>(result_irb.get<R"(scsw\count)">())});
                    return ::std::unexpected(
                        ::lib::kernel_error::from_driver(::lib::driver_error::io_error));
                }
                return result_irb;
            }

            if (cpu::clock::stck() > deadline) {
               const auto saved_irq = cpu::irq::irq_save_disable();

                if (entry->io_done.load(std::memory_order::seq_cst) != 0) {
                    irb result_irb{};
                    ::std::copy_n(reinterpret_cast<const u8*>(&entry->last_irb), sizeof(irb), result_irb.raw_ptr());
                    cpu::irq::irq_restore(saved_irq);
                    if (result_irb.get<"scsw">().has_device_error() || result_irb.get<"scsw">().has_sch_error())
                        return ::std::unexpected(
                            ::lib::kernel_error::from_driver(::lib::driver_error::io_error));
                    return result_irb;
                }

                irb result_irb{};
                const int tcc = css::tsch(entry->schid, result_irb);
                if (tcc == 0) {
                    const auto status_ctl = result_irb.get<R"(scsw\control)">();
                    const bool terminal = status_ctl.get<R"(status\primary)">() ||
                                          status_ctl.get<R"(status\secondary)">();
                    const bool inactive = !status_ctl.get<R"(activity\sch_active)">() &&
                                          !status_ctl.get<R"(activity\device_active)">();
                    if (terminal && inactive) {
                        ::std::copy_n(result_irb.raw_ptr(), sizeof(irb), reinterpret_cast<u8*>(&entry->last_irb));
                        entry->state = css::subchannel_state::idle;
                        cpu::irq::irq_restore(saved_irq);
                        if (result_irb.get<"scsw">().has_device_error() || result_irb.get<"scsw">().has_sch_error())
                            return ::std::unexpected(
                                ::lib::kernel_error::from_driver(::lib::driver_error::io_error));
                        return result_irb;
                    }
                }

                discard_value css::csch(entry->schid);
                irb discard{};
                discard_value css::tsch(entry->schid, discard);
                entry->state = css::subchannel_state::idle;
                cpu::irq::irq_restore(saved_irq);
                return ::std::unexpected(
                    ::lib::kernel_error::from_driver(::lib::driver_error::timeout));
            }

            cpu::sync::cpu_relax();
        }
    }

    /// @brief Issue SENSE ID on a subchannel to identify the attached device.
    /// @param sch_index Index into the subchannel table.
    /// @return senseid_data on success, kernel_error on failure.
    [[nodiscard]] auto sense_id(u32 sch_index) noexcept
        -> ::std::expected<senseid_data, ::lib::kernel_error>
    {
        using namespace zxfoundation::memory::pmm;
        using namespace zxfoundation::memory::hhdm;

        // Allocate one DMA31 page for CCW + data buffer.
        auto page_r = alloc(0, zone::dma31, purpose::device_dma, page_init::zeroed);
        if (!page_r) {
            return ::std::unexpected(page_r.error());
        }

        auto page = ::std::move(*page_r);
        const u64 phys = page.phys_base;
        const u64 vaddr = phys_to_virt(phys);

        // Place CCW at offset 0, data buffer at offset 256.
        ccw1::ref_type ccw(reinterpret_cast<void*>(vaddr));
        auto* buf = reinterpret_cast<senseid_data*>(vaddr + 256);

        ccw.set<"cmd_code">(ccw_commands::sense_id);
        ccw.set<R"(flags\suppress_length)">(true);
        ccw.set<"count">(static_cast<u16>(sizeof(senseid_data)));
        ccw.set<"data_addr">(static_cast<u32>(phys + 256)); // 31-bit physical address of buffer.

        auto io_r = sync(sch_index, static_cast<u32>(phys), 0, 500'000);

        senseid_data result{};
        if (io_r) {
            ::std::copy_n(reinterpret_cast<const u8*>(buf), sizeof(senseid_data), reinterpret_cast<u8*>(&result));
        }

        free(::std::move(page).make_active());

        if (!io_r) {
            return ::std::unexpected(io_r.error());
        }

        return result;
    }

    /// @brief Run SENSE ID on all probed subchannels to populate device type info.
    auto probe_devices() noexcept -> void {
        using namespace zxfoundation::sys::printk;

        const u32 count = css::subchannel_count();
        if (count == 0) {
            pinfo("ccw_io: no subchannels to probe");
            return;
        }

        pverbose("ccw_io: probing {} subchannel(s) with SENSE ID",
              ::lib::format::dec{static_cast<u64>(count)});

        u32 identified = 0;

        for (u32 i = 0; i < count; ++i) {
            auto sense_r = sense_id(i);
            if (!sense_r) {
                auto* entry = css::get_entry(i);
                pnotice("ccw_io: sch[{}] devno={} SENSE ID failed",
                      ::lib::format::dec{static_cast<u64>(i)},
                      ::lib::format::hex{static_cast<u64>(entry ? entry->devno : 0xFFFF), 4, true});
                continue;
            }

            auto* entry = css::get_entry(i);
            if (!entry) continue;

            // Store the sense data in the subchannel table entry.
            ::std::copy_n(reinterpret_cast<const u8*>(&(*sense_r)), sizeof(senseid_data), reinterpret_cast<u8*>(&entry->sense));
            ++identified;

            const u16 dev_type = sense_r->dev_type;
            const u16 cu_type  = sense_r->cu_type;

            // Look up both device and CU types in the device database.
            const auto m = s390x::css::devmatch::match(dev_type, cu_type);

            if (!m.dev_name.empty() && !m.cu_name.empty()) {
                pverbose("ccw_io: sch[{}] devno={} -> {} ({})",
                      ::lib::format::dec{static_cast<u64>(i)},
                      ::lib::format::hex{static_cast<u64>(entry->devno), 4, true},
                      m.dev_name, m.cu_name);
            } else if (!m.dev_name.empty()) {
                pverbose("ccw_io: sch[{}] devno={} -> {} (cu={})",
                      ::lib::format::dec{static_cast<u64>(i)},
                      ::lib::format::hex{static_cast<u64>(entry->devno), 4, true},
                      m.dev_name,
                      ::lib::format::hex{static_cast<u64>(cu_type), 4, true});
            } else if (!m.cu_name.empty()) {
                pverbose("ccw_io: sch[{}] devno={} -> dev={} ({})",
                      ::lib::format::dec{static_cast<u64>(i)},
                      ::lib::format::hex{static_cast<u64>(entry->devno), 4, true},
                      ::lib::format::hex{static_cast<u64>(dev_type), 4, true},
                      m.cu_name);
            } else {
                pverbose("ccw_io: sch[{}] devno={} -> dev={} cu={}",
                      ::lib::format::dec{static_cast<u64>(i)},
                      ::lib::format::hex{static_cast<u64>(entry->devno), 4, true},
                      ::lib::format::hex{static_cast<u64>(dev_type), 4, true},
                      ::lib::format::hex{static_cast<u64>(cu_type), 4, true});
            }
        }

        pinfo("ccw_io: SENSE ID complete, {}/{} device(s) identified",
              ::lib::format::dec{static_cast<u64>(identified)},
              ::lib::format::dec{static_cast<u64>(count)});

        if (identified != count) {
            pnotice("ccw_io: some devices are not identified");
        }
    }

} // namespace arch::s390x::io::ccw
