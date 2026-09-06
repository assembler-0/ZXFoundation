/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/init/main.cxx
/// @brief Kernel main entry point

import zxfoundation.base.types;
import zxfoundation.sys.time.core;
import zxfoundation.sys.timer.core;
import zxfoundation.sys.printk.core;
import zxfoundation.base.config;
import zxfoundation.memory.pmm;
import zxfoundation.sys.irq.core;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.service.core;
import zxfoundation.sys.zxio.core;
import zxfoundation.memory.hhdm;
import zxfoundation.memory.numa;
import zxfoundation.base.cxxrtse.cxxabi;
import zxfoundation.memory.slub;
import zxfoundation.sync.rcu.core;
import zxfoundation.memory.kmalloc;
import zxfoundation.memory.vm;
import zxfoundation.memory.objects;
import zxfoundation.dgp.cap.core;
import zxfoundation.dgp.cap.types;
import zxfoundation.scoms.kobject.base;
import zxfoundation.scoms.kobject.types;
import zxfoundation.memory.rmap;
import zxfoundation.memory.reclaim;
import zxfoundation.sys.ipc.core;
import zxfoundation.sched.core;
import zxfoundation.sched.strandctx.core;
import zxfoundation.sched.numa.core;
import zxfoundation.init.bootstrap.core;
import zxfoundation.init.storage.core;
import zxfoundation.dgp.domain.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.gate.core;
import zxfoundation.dgp.gate.registration;
import zxfoundation.dgp.portal;
import zxfoundation.dgp.isolation;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.policy;
import zxfoundation.base.cmdline.core;
import arch.s390x.init.zxfl.protocol;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.processor;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.psw;
import arch.s390x.cpu.smp;
import arch.s390x.cpu.features;
import arch.s390x.cpu.irq;
import arch.s390x.mmu.core;
import arch.s390x.cpu.aste;
import arch.s390x.io.css.subchannel;
import arch.s390x.io.ccw.core;
import crypto.sha256.core;
import drivers.console.diag.core;
import drivers.watchdog.core;
import lib.error;
import lib.bitfield;
import lib.stl;

using zxfoundation::sys::printk::pinfo;
using zxfoundation::sys::printk::perr;
using zxfoundation::sys::printk::pnotice;
using zxfoundation::sys::printk::pverbose;

namespace zxfoundation::init::detail {

    using namespace zxfoundation::memory::hhdm;
    using namespace zxfoundation::sys::syschk;

    /// @brief Verify the boot protocol magic and binding token.
    auto verify_protocol_integrity(const arch::s390x::init::zxfl::protocol& boot) -> void {
        if (boot.magic != arch::s390x::init::zxfl::ZXFL_MAGIC)
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol),
                "sys: invalid protocol magic"
            );

        const u64 expected = arch::s390x::init::zxvl::compute_token(
            boot.stfle_fac[0], boot.ipl_schid);
        if (boot.binding_token != expected)
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::checksum_mismatch),
                "sys: invalid binding token"
            );
    }

    /// @brief Validate the loader-written stack frame canary at kernel_stack_top.
    auto validate_stack_frame(const arch::s390x::init::zxfl::protocol& boot) -> void {
        auto bad = [&] [[noreturn]] -> void {
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::checksum_mismatch),
                "sys: stack frame corruption - unauthorized loader"
            );
        };

        if (!boot.kernel_stack_top) [[unlikely]] bad();

        const auto* frame = reinterpret_cast<const u64*>(boot.kernel_stack_top);
        if (frame[0] != arch::s390x::init::zxvl::ZXVL_FRAME_MAGIC_A) bad();

        const u64 expected_b =
            arch::s390x::init::zxvl::ZXVL_FRAME_MAGIC_B ^ boot.binding_token;
        if (frame[1] != expected_b) bad();
    }

    /// @brief Re-verify all kernel segment checksums from the HHDM-mapped image.
    auto verify_kernel_checksums(const arch::s390x::init::zxfl::protocol& boot) -> void {
        if (!boot.cksum_table) [[unlikely]]
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol),
                "sys: no kernel checksum table — unauthorized loader"
            );

        const auto* tbl = reinterpret_cast<const arch::s390x::init::zxvl::checksum_table*>(
            boot.cksum_table);

        if (tbl->magic != arch::s390x::init::zxvl::ZXVL_CKSUM_MAGIC)
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol),
                "sys: nucleus checksum table absent or corrupt"
            );

        if (tbl->version != arch::s390x::init::zxvl::ZXVL_CKSUM_VERSION)
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol),
                "sys: nucleus checksum table version mismatch"
            );

        if (tbl->count == 0U || tbl->count > arch::s390x::init::zxvl::ZXVL_CKSUM_MAX_ENTRIES)
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol),
                "sys: nucleus checksum table entry count invalid"
            );

        if (tbl->algo != arch::s390x::init::zxvl::ZXVL_CKSUM_ALGO_SHA256)
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol),
                "sys: nucleus checksum algorithm unrecognized"
            );

        for (u32 i = 0U; i < tbl->count; ++i) {
            const auto& [phys_start, size, digest] = tbl->entries[i];
            if (size == 0U) continue;

            const u64 virt_seg = phys_to_virt(phys_start);
            const std::span data{reinterpret_cast<const u8*>(virt_seg), size};
            const std::span<const u8, crypto::sha256::SHA256_DIGEST_SIZE>
                expected{digest, crypto::sha256::SHA256_DIGEST_SIZE};

            if (const auto r = crypto::sha256::sha256_verify(data, expected); !r)
                syschk_fatal(
                    lib::kernel_error::from_init(lib::init_error::code::checksum_mismatch),
                    "sys: nucleus checksum mismatch - unauthorized loader"
                );
        }
    }

    /// @brief Verify loader identity fingerprint against the embedded expected digest.
    auto verify_loader_fingerprint(const arch::s390x::init::zxfl::protocol& boot) -> void {
        const std::span<const u8, crypto::sha256::SHA256_DIGEST_SIZE>
            claimed_digest(boot.loader_digest, crypto::sha256::SHA256_DIGEST_SIZE);

        constexpr std::span<const u8, crypto::sha256::SHA256_DIGEST_SIZE>
            expected_digest(arch::s390x::init::zxvl::ZXFL_EXPECTED_DIGEST,
                            crypto::sha256::SHA256_DIGEST_SIZE);

        bool expected_zero = true;
        for (const auto b : expected_digest) {
            if (b != 0U) { expected_zero = false; break; }
        }
        if (expected_zero) return;

        for (u32 i = 0U; i < crypto::sha256::SHA256_DIGEST_SIZE; ++i) {
            if (claimed_digest[i] != expected_digest[i]) [[unlikely]]
                syschk_fatal(
                    lib::kernel_error::from_init(lib::init_error::code::checksum_mismatch),
                    "sys: loader identity rejected - embedded expected digest mismatch"
                );
        }
    }

    /// @brief Print the kernel banner.
    auto banner(const arch::s390x::init::zxfl::protocol& proto) -> void {
        pinfo("sys: ZXFoundation (TM) {} - Copyright (c) {} assembler-0. Licensed under the Apache License, Version 2.0.\n",
            zxfoundation::base::config::CONFIG_ZX_RELEASE,
            zxfoundation::base::config::CONFIG_ZX_COPYRIGHT_DATE);

        pinfo("sys: built on {} - {} with {}. build no #{} \n",
            zxfoundation::base::config::CONFIG_ZX_BUILD_DT_COMPILER,
            zxfoundation::base::config::CONFIG_ZX_BUILD_PLATFORM,
            zxfoundation::base::config::CONFIG_ZX_COMPILER_ID,
            zxfoundation::base::config::CONFIG_ZX_BUILD_NUMBER);
        pinfo("cmdline: {}", reinterpret_cast<const char*>(proto.cmdline_addr));
    }

    /// @brief Initialize the HHDM coverage contract exported by ZXFL.
    /// @param[in] boot Loader protocol containing the active direct-map coverage evidence.
    auto initialize_hhdm_contract(const arch::s390x::init::zxfl::protocol& boot) -> void {
        if (boot.hhdm_phys_coverage_end == 0U
            || (boot.hhdm_phys_coverage_end & (4096ULL - 1ULL)) != 0U
            || boot.hhdm_phys_coverage_end < boot.pgtbl_pool_end
            || boot.hhdm_phys_coverage_end < boot.kernel_phys_end) [[unlikely]] {
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol),
                "sys: invalid ZXFL HHDM coverage contract");
        }

        const auto* map = reinterpret_cast<const arch::s390x::init::zxfl::mem_region*>(
            boot.mem_map_addr);
        if (boot.mem_map_count > arch::s390x::init::zxfl::ZXFL_MEM_MAP_MAX
            || (boot.mem_map_count != 0U && !map)) [[unlikely]] {
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol),
                "sys: invalid ZXFL memory-map descriptor");
        }
        u64 highest_ram_end = 0U;
        for (u32 i = 0U; i < boot.mem_map_count; ++i) {
            if (map[i].length != 0U
                && map[i].base > ~0ULL - map[i].length) [[unlikely]] {
                syschk_fatal(
                    lib::kernel_error::from_init(lib::init_error::code::invalid_protocol),
                    "sys: ZXFL memory-map range overflow at entry {}", i);
            }
            if (const u64 region_end = map[i].base + map[i].length;
                region_end > highest_ram_end) highest_ram_end = region_end;
        }
        if (highest_ram_end > boot.hhdm_phys_coverage_end) [[unlikely]] {
            syschk_fatal(
                lib::kernel_error::from_init(lib::init_error::code::invalid_protocol),
                "sys: installed RAM exceeds ZXFL HHDM direct-map coverage");
        }

        set_phys_coverage_end(boot.hhdm_phys_coverage_end);
    }

    /// @brief Initialize optional NUMA topology without making topology evidence a boot blocker.
    auto initialize_numa_topology(const arch::s390x::init::zxfl::protocol& boot) -> void {
        if (!zxfoundation::memory::numa::init(boot)) {
            pnotice("sys: NUMA topology unavailable; continuing with allocator bootstrap fallback\n");
            return;
        }

        if (zxfoundation::memory::numa::node_count() == 0U) [[unlikely]] {
            pnotice("sys: NUMA topology empty; allocator will use bootstrap fallback\n");
        }
        if (!zxfoundation::memory::numa::has_loader_memory_evidence()) {
            pnotice("sys: NUMA topology has no ZXFL memory-map node records; kernel self-discovery should refine placement later\n");
        }
        if (zxfoundation::memory::numa::uses_synthetic_fallback()) {
            pnotice("sys: NUMA boot node synthesized from available boot evidence\n");
        }
    }

    auto launch_bootstrap_domain(const arch::s390x::init::zxfl::protocol& proto) -> void  {
        auto dom_r = zxfoundation::dgp::domain::domain_create("foundation/bootstrapd");
        if (!dom_r) {
            zxfoundation::sys::printk::pcrit(
                "sys: cannot launch bootstrap domain: {}", dom_r.error());
            return;
        }
        auto* d = *dom_r;
        d->granted_rights = zxfoundation::dgp::isolation::isolation_rights::all;
        d->primary_strand.cpu = 0;
        zxfoundation::sched::strandctx::setup_primary_strand_context(
            d,
            zxfoundation::init::bootstrap::bootstrap_entry,
            const_cast<void*>(static_cast<const void*>(&proto)));
        zxfoundation::sched::enqueue_domain(d);
    }

    auto cmdline_init(const arch::s390x::init::zxfl::protocol& proto) -> void {
        zxfoundation::base::cmdline::add({
            .name = "printk.min_level",
            .type = zxfoundation::base::cmdline::value_type::callback,
            .handler = zxfoundation::sys::printk::handle_min_level,
            .help = "Set minimum log level (0-7, 7=verbose)"
        });

        zxfoundation::base::cmdline::parse({
            reinterpret_cast<const char*>(proto.cmdline_addr),
            proto.cmdline_len
        });
    }

    auto cap_register_lookup() -> void {
        discard_value zxfoundation::dgp::cap::register_lookup(
        zxfoundation::dgp::cap::cap_type::memory_extent,
        [](u16 object_id) noexcept -> zxfoundation::scoms::kobject::bobject* {
            auto* object = zxfoundation::memory::objects::memory_object_resolve_id(
                static_cast<u32>(object_id));
            return object && object->ko.type == zxfoundation::scoms::kobject::kobject_type::memory_extent
                ? &object->ko : nullptr;
        });
    }

    auto rmap_init() -> void {
        if (auto rmap_init = zxfoundation::memory::rmap::init(
            zxfoundation::memory::pmm::max_pfn()); !rmap_init) {
            zxfoundation::sys::syschk::syschk_fatal(
                rmap_init.error(), "sys: rmap initialization failed");
            }
    }

} // namespace detail

/// @brief nucleus main entry point
declscope(C) [[noreturn]] auto zxfoundation_global_initialize(
    const arch::s390x::init::zxfl::protocol* protocol
) noexcept -> void {
    if (!protocol) [[unlikely]]
        arch::s390x::cpu::psw::disabled_wait();

    using namespace zxfoundation::init;

    detail::verify_protocol_integrity(*protocol);
    detail::validate_stack_frame(*protocol);
    detail::verify_kernel_checksums(*protocol);
    detail::verify_loader_fingerprint(*protocol);

    zxfoundation::base::cxxrtse::cxxabi::run_global_constructor();
    zxfoundation::base::cxxrtse::cxxabi::ssp_init();

    storage::set_protocol(*protocol);

    zxfoundation::sys::printk::init();
    zxfoundation::sys::printk::register_backend(drivers::console::diag::get_backend());
    zxfoundation::sys::syschk::init(*protocol);
    detail::cmdline_init(*protocol);
    detail::banner(*protocol);

    arch::s390x::cpu::features::init(std::nullopt);
    arch::s390x::cpu::features::enable_hardware_facilities();
    arch::s390x::cpu::lowcore::init_bsp(*protocol);
    arch::s390x::cpu::percpu::init_bsp();
    zxfoundation::sys::time::initialize(protocol->tod_boot);

    detail::initialize_hhdm_contract(*protocol);
    detail::initialize_numa_topology(*protocol);
    zxfoundation::memory::pmm::init(*protocol);
    arch::s390x::mmu::init();

    zxfoundation::memory::slub::slub_init();
    zxfoundation::memory::kmalloc::kmalloc_init();
    zxfoundation::memory::vm::vm_init();
    zxfoundation::memory::objects::memory_objects_init();
    detail::cap_register_lookup();
    zxfoundation::memory::slub::slub_attach_authorities();
    detail::rmap_init();
    zxfoundation::sys::ipc::ipc_init();
    zxfoundation::dgp::policy::init();

    zxfoundation::dgp::isolation::init();

    zxfoundation::sys::irq::init();
    zxfoundation::sys::timer::init();

    zxfoundation::sync::rcu::init();
    arch::s390x::cpu::aste::aste_subsystem_init();

    zxfoundation::dgp::domain::domain_init();
    discard_value zxfoundation::dgp::domain::domain_init_nucleus();
    zxfoundation::dgp::domain::strand_init();
    zxfoundation::sys::service::init();
    zxfoundation::dgp::gate::gate_init();
    zxfoundation::dgp::gate::registration::register_all_handlers();
    zxfoundation::dgp::portal::init();

    zxfoundation::sched::sched_init();

    zxfoundation::sched::numa::slit_load();
    arch::s390x::cpu::smp::launch(*protocol);
    arch::s390x::cpu::irq::irq_enable();

    arch::s390x::io::css::init();
    arch::s390x::io::ccw::probe_devices();
    zxfoundation::sys::zxio::init();

    detail::launch_bootstrap_domain(*protocol);

    pinfo("sys: core.zxfoundation.nucleus initialization complete");

    drivers::watchdog::init();

    zxfoundation::sched::idle_enter();
}
