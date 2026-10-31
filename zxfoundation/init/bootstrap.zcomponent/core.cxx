/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/init/bootstrap.zcomponent/core.cxx
/// @brief sys/bootstrapd — first schedulable kernel domain.

module zxfoundation.init.bootstrap.core;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.scoms.kobject.ref;
import zxfoundation.dgp.cap.types;
import zxfoundation.dgp.cap.core;
import zxfoundation.sys.printk.core;
import zxfoundation.sys.syschk.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.domain.core;
import zxfoundation.dgp.isolation.types;
import zxfoundation.sched.core;
import zxfoundation.sched.topology.core;
import zxfoundation.sched.topology.types;
import zxfoundation.sched.strandctx.core;
import zxfoundation.memory.hhdm;
import zxfoundation.memory.numa;
import arch.s390x.cpu.lowcore;
import arch.s390x.cpu.psw;
import arch.s390x.init.zxfl.protocol;
import fs.exec.elf.core;
import lib.error;
import lib.cstring;
import lib.stl;

using zxfoundation::sys::printk::pinfo;
using zxfoundation::sys::printk::perr;
using zxfoundation::sys::printk::pverbose;

namespace zxfoundation::init::bootstrap {

    /// @brief Entry function for the sys/bootstrap domain.
    /// @param arg   Pointer to the boot @c protocol descriptor
    auto bootstrap_entry(void* arg) noexcept -> void {
        if (!arg) goto exit;
        {
            pinfo("bootstrap: searching for init module...");
            
            const arch::s390x::init::zxfl::protocol* proto = static_cast<arch::s390x::init::zxfl::protocol*>(arg);
            const arch::s390x::init::zxfl::sysmodule* found = nullptr;

            for (u32 i = 0; i < proto->module_count && i < 16U; ++i) {
                const auto& mod = proto->modules[i];
                if (std::string_view(mod.name) == "ZXF.INIT") {
                    found = &mod;
                    break;
                }
            }

            // Fall back to the first module if only one is loaded.
            if (!found && proto->module_count == 1U) {
                found = &proto->modules[0];
                pinfo("bootstrap: using first module as init (name=\"{}\")", found->name);
            }

            if (!found || found->size_bytes == 0 || found->phys_start == 0) {
                perr("bootstrap: no init module found/corrupted module — system cannot continue");
                auto* self = static_cast<zxfoundation::dgp::domain::domain*>(
                    arch::s390x::cpu::lowcore::current_domain_ptr());
                if (self) {
                    discard_value scoms::kobject::mark_dying(self);
                }
                zxfoundation::sched::schedule();
                return;
            }

            pverbose("bootstrap: found init module phys={:#x} size={}", found->phys_start, found->size_bytes);

            const u64 data_virt = zxfoundation::memory::hhdm::phys_to_virt(found->phys_start);

            auto dom_r = zxfoundation::dgp::domain::domain_create("user/init");
            if (!dom_r) {
                perr("bootstrap: failed to create user/init domain");
                auto* self = static_cast<zxfoundation::dgp::domain::domain*>(
                    arch::s390x::cpu::lowcore::current_domain_ptr());
                if (self) {
                    discard_value scoms::kobject::mark_dying(self);
                }
                zxfoundation::sched::schedule();
                return;
            }
            auto* d = *dom_r;

            d->granted_rights = ::zxfoundation::dgp::right::all;
            d->primary_strand.cpu = 0;

            auto entry_r = fs::exec::elf::load(
                d,
                reinterpret_cast<const void*>(data_virt),
                found->size_bytes);

            if (!entry_r) [[unlikely]] {
                perr("bootstrap: load failed for user/init: {}", entry_r.error());
                discard_value zxfoundation::scoms::kobject::mark_dying(d);
                zxfoundation::sched::schedule();
                return;
            }

            const u64 entry_point = *entry_r;

            d->primary_strand.body->uregs.psw_mask = arch::s390x::cpu::psw::PSW_MASK_USER;
            d->primary_strand.body->uregs.psw_addr = entry_point;

            {
                auto admin_r = zxfoundation::dgp::cap::cap_issue(
                    zxfoundation::dgp::cap::cap_type::domain,
                    static_cast<u16>(d->ko.id),
                    ::zxfoundation::dgp::right::all,
                    d->ko.id);
                if (!admin_r) {
                    perr("bootstrap: failed to issue init admin capability");
                    discard_value zxfoundation::scoms::kobject::mark_dying(d);
                    zxfoundation::sched::schedule();
                    return;
                }
                d->admin_token = admin_r->raw;
                d->primary_strand.body->uregs.gprs[10] = d->admin_token;
                pverbose("bootstrap: admin token {:#016x} set for domain {} gen={}",
                      d->admin_token, d->ko.id, static_cast<u32>(d->ko.generation));
            }

            {
                auto stack_result = zxfoundation::sched::strandctx::
                    seed_primary_linkage_stack(d);
                if (!stack_result) [[unlikely]] {
                    perr("bootstrap: linkage stack seed failed: {}",
                         stack_result.error());
                    discard_value zxfoundation::scoms::kobject::mark_dying(d);
                    zxfoundation::sched::schedule();
                    return;
                }
            }
            zxfoundation::sched::strandctx::setup_primary_strand_context_user(d);
            zxfoundation::sched::enqueue_domain(d);

        }

    exit:

        auto* self = static_cast<zxfoundation::dgp::domain::domain*>(
            arch::s390x::cpu::lowcore::current_domain_ptr());
        if (self) {
            discard_value scoms::kobject::mark_dying(self);
        }
        zxfoundation::sched::schedule();
    }

} // namespace zxfoundation::init::bootstrap
