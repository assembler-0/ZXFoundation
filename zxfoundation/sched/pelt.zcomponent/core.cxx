/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/pelt.zcomponent/core.cxx
/// @brief Per-Entity Load Tracking (PELT) — decayed-load averaging.

module zxfoundation.sched.pelt.core;
import zxfoundation.sched.entity;
import zxfoundation.base.types;

namespace zxfoundation::sched::pelt {

    /// @brief Accumulate a signed-delta running contribution into a PELT
    ///        averaging record, advancing by zero or more full periods.
    /// @param[in]     delta_ns  Elapsed time since the prior update (in ns).
    /// @param[in,out] sa        Averaging record (mutable).
    /// @param[in]     load      Load contribution (0 or se->weight average).
    /// @param[in]     runnable  Runnable mask (1 if runnable, 0 if blocked).
    /// @param[in]     running   Running mask (1 if on CPU, 0 if not).
    /// @return 1 when the averaging record was updated, 0 when delta=0 or
    ///         nothing changed.
    auto accumulate_sum(u64 delta_ns, sched_avg& sa, u32 load,
                        u32 runnable, u32 running) noexcept -> u32 {
        if (delta_ns == 0U) return 0U;
        if (delta_ns >= (PELT_HALF_LIFE * 4ULL) * PELT_PERIOD_NS) {
            sa.load_sum = load;
            sa.runnable_sum = runnable;
            sa.util_sum = running;
            sa.period_contrib_ns = 0U;
            sa.last_update_time_ns += delta_ns;
            return 1U;
        }
        u64 periods = delta_ns / PELT_PERIOD_NS;
        u64 residual = delta_ns % PELT_PERIOD_NS;
        if (periods > 0U) {
            sa.load_sum = decay_load(sa.load_sum, periods);
            if (runnable == 0U) {
                sa.runnable_sum = decay_load(sa.runnable_sum, periods);
            }
            if (running == 0U) {
                sa.util_sum = decay_load(sa.util_sum, periods);
            }
        }
        const u64 contribution = residual * 1024U + (sa.period_contrib_ns * 1024U) / PELT_PERIOD_NS;
        const u32 contrib = static_cast<u32>((contribution * 1024U) >> 10U);
        if (load != 0U)       sa.load_sum      += contrib * load;
        if (runnable != 0U)   sa.runnable_sum   += contrib;
        if (running  != 0U)   sa.util_sum       += contrib;
        sa.period_contrib_ns = residual;
        sa.last_update_time_ns += delta_ns;
        return 1U;
    }

    /// @brief Fold the decayed sums into the public averages.
    /// @param[in,out] sa     Averaging record (mutable).
    /// @param[in]     weight Entity weight (1024 for default nice-0).
    auto update_load_avg(sched_avg& sa, u32 weight) noexcept -> void {
        if (sa.load_sum > LOAD_AVG_MAX)  sa.load_sum = LOAD_AVG_MAX;
        if (sa.runnable_sum > LOAD_AVG_MAX) sa.runnable_sum = LOAD_AVG_MAX;
        if (sa.util_sum > LOAD_AVG_MAX) sa.util_sum = LOAD_AVG_MAX;
        const u32 w = (weight == 0U) ? 1024U : weight;
        // load_avg = DIV64_U64_ROUND_UP(load_sum * w) / LOAD_AVG_MAX
        sa.load_avg = (sa.load_sum * w) / LOAD_AVG_MAX;
        sa.runnable_avg = sa.runnable_sum / LOAD_AVG_MAX;
        sa.util_avg = sa.util_sum / LOAD_AVG_MAX;
    }

    /// @brief Drive a full per-entity PELT update cycle.
    /// @param[in,out] sa      Entity averaging record (mutable).
    /// @param[in]     now_ns  Monotonic timestamp (s_t clock-derived).
    /// @param[in]     weight  Entity weight (1024 for nice 0).
    /// @param[in]     runnable 1 if entity is on a runqueue, 0 otherwise.
    /// @param[in]     running  1 if entity is currently running, 0 otherwise.
    auto update_entity_load_avg(sched_avg& sa, u64 now_ns,
                                u32 weight, u32 runnable, u32 running) noexcept -> void {
        const u64 delta_ns = (now_ns > sa.last_update_time_ns)
            ? (now_ns - sa.last_update_time_ns) : 0U;
        if (delta_ns == 0U) return;
        if (accumulate_sum(delta_ns, sa, weight ? weight : 1024U, runnable, running) != 0U) {
            update_load_avg(sa, weight ? weight : 1024U);
        }
    }

    /// @brief Drive a full per-RQ PELT update cycle for @c sched_avg_rq.
    auto update_rq_load_avg(sched_avg_rq& rq, u64 now_ns, u32 runnable) noexcept -> void {
        const u64 delta_ns = (now_ns > rq.last_update_time_ns)
            ? (now_ns - rq.last_update_time_ns) : 0U;
        if (delta_ns == 0U) return;
        if (delta_ns >= (PELT_HALF_LIFE * 4ULL) * PELT_PERIOD_NS) {
            rq.load_sum = runnable;
            rq.runnable_sum = runnable;
            rq.util_sum = runnable;
            rq.period_contrib_ns = 0U;
            rq.last_update_time_ns += delta_ns;
        } else {
            u64 periods = delta_ns / PELT_PERIOD_NS;
            u64 residual = delta_ns % PELT_PERIOD_NS;
            if (periods > 0U) {
                rq.load_sum = decay_load(rq.load_sum, periods);
                if (runnable == 0U) {
                    rq.runnable_sum = decay_load(rq.runnable_sum, periods);
                    rq.util_sum = decay_load(rq.util_sum, periods);
                }
            }
            const u64 contribution = residual * 1024U + (rq.period_contrib_ns * 1024U) / PELT_PERIOD_NS;
            rq.load_sum += contribution * runnable;
            rq.runnable_sum += contribution * runnable;
            rq.util_sum += contribution * runnable;
            rq.period_contrib_ns = residual;
            rq.last_update_time_ns += delta_ns;
        }
        if (rq.load_sum > LOAD_AVG_MAX) rq.load_sum = LOAD_AVG_MAX;
        if (rq.runnable_sum > LOAD_AVG_MAX) rq.runnable_sum = LOAD_AVG_MAX;
        if (rq.util_sum > LOAD_AVG_MAX) rq.util_sum = LOAD_AVG_MAX;
        rq.load_avg = rq.load_sum / LOAD_AVG_MAX;
        rq.runnable_avg = rq.runnable_sum / LOAD_AVG_MAX;
        rq.util_avg = rq.util_sum / LOAD_AVG_MAX;
    }

} // namespace zxfoundation::sched::pelt