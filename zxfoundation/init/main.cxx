/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/init/main.cxx
/// @brief Kernel main entry point.

import zxfoundation.base.types;
import zxfoundation.sys.printk.core;
import zxfoundation.base.config;
import zxfoundation.sys.syschk.core;
import zxfoundation.sys.zinit;
import zxfoundation.sys.zinitgraph;
import zxfoundation.sync.lockgraph.core;
import zxfoundation.base.cxxrtse.cxxabi;
import zxfoundation.sched.core;
import zxfoundation.sched.strandctx.core;
import zxfoundation.init.bootstrap.core;
import zxfoundation.init.storage.core;
import zxfoundation.dgp.domain.core;
import zxfoundation.dgp.isolation.types;
import zxfoundation.memory.hhdm.core;
import zxfoundation.base.cmdline.core;
import arch.s390x.init.zxfl.protocol;
import crypto.sha256.core;
import lib.error;
import lib.stl;

#define __zxmacro_want_assert
#define __zxmacro_want_zinit
#include macro_extensions

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
    no_export auto banner() -> void {
        const auto& proto = zxfoundation::init::storage::get_protocol();
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

    /// @brief Launch the bootstrap domain.
    no_export auto launch_bootstrap_domain() -> void {
        const auto& proto = zxfoundation::init::storage::get_protocol();
        auto dom_r = zxfoundation::dgp::domain::domain_create("foundation/bootstrapd");
        if (!dom_r) {
            zxfoundation::sys::printk::pcrit(
                "sys: cannot launch bootstrap domain: {}", dom_r.error());
            return;
        }
        auto* d = *dom_r;
        d->granted_rights = ::zxfoundation::dgp::right::all;
        d->primary_strand.cpu = 0;
        zxfoundation::sched::strandctx::setup_primary_strand_context(
            d,
            zxfoundation::init::bootstrap::bootstrap_entry,
            const_cast<void*>(static_cast<const void*>(&proto)));
        zxfoundation::sched::enqueue_domain(d);
    }

    /// @brief Register command line parameters and parse the boot command line.
    auto cmdline_init(const arch::s390x::init::zxfl::protocol& proto) -> void {
        zxfoundation::base::cmdline::add({
            .name = "zx.sys.printk.min_level",
            .type = zxfoundation::base::cmdline::value_type::callback,
            .handler = zxfoundation::sys::printk::handle_min_level,
            .help = "Set minimum log level (0-7, 7=verbose)"
        });

        zxfoundation::sys::zinitgraph::register_cmdline();
        zxfoundation::sync::lockgraph::register_cmdline();

        zxfoundation::base::cmdline::parse({
            reinterpret_cast<const char*>(proto.cmdline_addr),
            proto.cmdline_len
        });
    }

} // namespace detail

namespace zxfoundation::init::nodes {

    using zxfoundation::sys::zinit::zi_desc;
    using zxfoundation::sys::zinit::zi_result;

    static auto zi_banner(const zi_desc&) noexcept -> zi_result {
        detail::banner();
        return {};
    }
    __zinit__(early, p50, zi_banner, warn)

} // namespace zxfoundation::init::nodes

/// @brief nucleus main entry point
declscope(C) [[noreturn]] auto zxfoundation_global_initialize(
    const arch::s390x::init::zxfl::protocol* protocol
) noexcept -> void {
    trap_on(!protocol);

    using namespace zxfoundation::init;
    using namespace zxfoundation::sys;

    detail::verify_protocol_integrity(*protocol);
    detail::verify_kernel_checksums(*protocol);
    detail::verify_loader_fingerprint(*protocol);

    zxfoundation::base::cxxrtse::cxxabi::init();
    storage::set_protocol(*protocol);
    detail::cmdline_init(*protocol);

    if (zinitgraph::enabled()) {
        zinitgraph::zg_graph graph{};
        if (auto br = zinitgraph::build(graph); !br) {
            perr("sys: initgraph construction failed ({}); falling back to flat engine",
                 br.error());
            if (const u32 failed = zinit::run_all(); failed != 0U)
                perr("sys: {} init entrie(s) reported failure", failed);
        } else {
            const u32 failed = zinitgraph::run(graph);
            if (graph.unreached != 0U)
                perr("sys: initgraph left {} node(s) unreached", graph.unreached);
            if (failed != 0U)
                perr("sys: {} init entrie(s) reported failure", failed);
        }
    } else {
        if (const u32 failed = zinit::run_all(); failed != 0U)
            perr("sys: {} init entrie(s) reported failure", failed);
    }

    pinfo("sys: core.zxfoundation.nucleus initialization complete");

    detail::launch_bootstrap_domain();
    zxfoundation::sched::idle_enter();
}
