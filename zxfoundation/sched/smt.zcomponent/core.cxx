/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/smt.cxx
/// @brief SMT-aware strand-group or co-scheduling decisions

module zxfoundation.sched.smt.core;
import zxfoundation.sched.topology.types;
import zxfoundation.sched.topology.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.dgp.isolation.types;
import zxfoundation.sched.policy;
import zxfoundation.base.types;
import arch.s390x.cpu.percpu;
import arch.s390x.cpu.cpumask;
import lib.cpumask;
import lib.error;
import lib.stl;

namespace zxfoundation::sched::smt {

    /// @brief Check whether two strands are eligible to share an SMT core
    [[nodiscard]] auto sibling_ok(const zxfoundation::dgp::domain::strand& a,
                                       const zxfoundation::dgp::domain::strand& b) noexcept -> bool {
        const auto* dom_a = static_cast<const zxfoundation::dgp::domain::domain*>(a.owner);
        const auto* dom_b = static_cast<const zxfoundation::dgp::domain::domain*>(b.owner);
        if (!dom_a || !dom_b) return true;  // idle always shared
        // If either domain prohibits SMT sibling sharing, separate them.
        if (!a.sched_policy.allow_smt_sibling || !b.sched_policy.allow_smt_sibling) return false;
        return true;
    }

    /// @brief Return a mask of SMT siblings of @c cpu that are currently online
    ///        and have no running strand (are idle).
    /// @param[in] cpu  Source CPU.
    /// @param[out] out_f candidates mask filled by the function.
    /// @note O(CPUS) walk; filtered by @c cpu_relation::same_smt_core only.
    auto sibling_idle_candidates(u16 cpu, ::lib::cpumask& out_f) noexcept -> void {
        out_f.reset();
        const auto* rec = zxfoundation::sched::topology::record_for(cpu);
        if (!rec || rec->smt_evidence == zxfoundation::sched::topology_evidence::degraded) return;
        zxfoundation::sched::topology::for_each_steal_candidate(
            cpu,
            zxfoundation::sched::topology::cpu_relation::same_smt_core,
            [&](u16 y) noexcept -> void {
                if (!arch::s390x::cpu::cpumask::cpu_online(y)) return;
                const auto* yr = zxfoundation::sched::topology::record_for(y);
                if (!yr) return;
                if (yr->run_queue_load == 0U) {
                    out_f.set(y, std::memory_order::relaxed);
                }
            }
        );
    }

    /// @brief Return True when the requested strand placement to a candidate CPU
    ///        would violate DGP isolation-sensitive SMT separation
    /// @param[in] strand  Strand under consideration.
    /// @param[in] target_cpu Candidate CPU.
    /// @note Accepts the strand by designer; consults the target's core sibling list.
    [[nodiscard]] auto isolation_violation(const zxfoundation::dgp::domain::strand& strand,
                                                u16 target_cpu) noexcept -> bool {
        const auto* my_rec = zxfoundation::sched::topology::record_for(target_cpu);
        if (!my_rec) return true;  // unknown CPU → conservatively deny

        const auto* my_dom =
            static_cast<const zxfoundation::dgp::domain::domain*>(strand.owner);

        const bool wants_isolation = my_dom
            && !zxfoundation::dgp::isolation::has_all_rights(
                my_dom->granted_rights,
                zxfoundation::dgp::isolation::isolation_rights::sched);
        if (my_rec->smt_evidence == zxfoundation::sched::topology_evidence::degraded
            && wants_isolation) {
            return true;
        }
        bool violated = false;
        zxfoundation::sched::topology::for_each_steal_candidate(
            target_cpu,
            zxfoundation::sched::topology::cpu_relation::same_smt_core,
            [&](u16 y) noexcept -> void {
                if (violated) return;
                const auto* pcp = arch::s390x::cpu::percpu::cpu_percpu(y);
                if (!pcp) return;
                const u32 attached_dom = pcp->attached_domain_id;
                if (my_dom && attached_dom != 0U && attached_dom != my_dom->ko.id) {
                    violated = true;
                }
            }
        );
        return violated;
    }

} // namespace zxfoundation::sched::smt