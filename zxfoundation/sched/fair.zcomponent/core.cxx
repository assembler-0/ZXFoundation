/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/sched/fair.cxx
/// @brief EEVDF (Earliest Eligible Virtual Deadline First) scheduling class.

module zxfoundation.sched.fair.core;
import zxfoundation.sched.entity;
import zxfoundation.sched.classed.core;
import zxfoundation.sched.smt.core;
import zxfoundation.dgp.domain.types;
import zxfoundation.base.types;
import lib.rbtree;
import lib.error;
import lib.container_of;
import lib.stl;

namespace zxfoundation::sched::fair {

    using zxfoundation::sched::sched_entity;

    /// @brief Scaled delta for calc_delta_fair: weighted virtual time advance.
    /// @param[in] delta  Wall-time delta (ns-equivalent or TOD ticks).
    /// @param[in] se     Schedulable entity.
    /// @return The weighted vruntime delta = delta * WEIGHT_MAX / se->weight.
    /// @note A heavier entity advances its vruntime slower (so it gets a
    ///       proportionally larger slice).
    [[nodiscard]] auto calc_delta_fair(u64 delta, const sched_entity& se) noexcept -> u64 {
        const u32 w = (se.weight == 0U) ? NICE_0_WEIGHT : se.weight;
        if (w == NICE_0_WEIGHT) return delta;
        const u128 product = static_cast<u128>(delta) * static_cast<u128>(WEIGHT_MAX);
        return static_cast<u64>(product / w);
    }

    /// @brief Compute the average vruntime across the queued entities.
    /// @param[in] cfs_min_vruntime  Current min_vruntime of the cfs_rq.
    /// @param[in] avg_vruntime_acc   Accumulator keeping weighted average.
    /// @param[in] avg_weight_acc     Accumulator keeping total weight.
    /// @return The current avg vruntime (in vruntime units).
    [[nodiscard]] auto avg_vruntime(u64 cfs_min_vruntime,
                                    i64 avg_vruntime_acc,
                                    u64 avg_weight_acc) noexcept -> u64 {
        if (avg_weight_acc == 0U) return cfs_min_vruntime;
        const i128 acc = static_cast<i128>(avg_vruntime_acc);
        const i128 wgt = static_cast<i128>(avg_weight_acc);
        const i128 avg = acc / wgt;
        return static_cast<u64>(static_cast<i64>(avg) + static_cast<i64>(cfs_min_vruntime));
    }

    /// @brief Update the running avg_vruntime accumulator when an entity is
    ///        added to the cfs_rq.
    /// @param[in]     se         Entity being inserted.
    /// @param[in,out] avg_acc    Mutable accumulator (i64, signed).
    /// @param[in,out] avg_w_acc  Mutable total weight accumulator (u64).
    auto avg_vruntime_add(const sched_entity& se, u64 cfs_min_vruntime,
                          i64& avg_acc, u64& avg_w_acc) noexcept -> void {
        const u32 w = (se.weight == 0U) ? NICE_0_WEIGHT : se.weight;
        const i64 normalized = static_cast<i64>(se.vruntime) - static_cast<i64>(cfs_min_vruntime);
        avg_acc += static_cast<i64>(w) * normalized;
        avg_w_acc += w;
    }

    /// @brief Update the running avg_vruntime accumulator when an entity is
    ///        removed from the cfs_rq.
    auto avg_vruntime_sub(const sched_entity& se, u64 cfs_min_vruntime,
                          i64& avg_acc, u64& avg_w_acc) noexcept -> void {
        const u32 w = (se.weight == 0U) ? NICE_0_WEIGHT : se.weight;
        const i64 normalized = static_cast<i64>(se.vruntime) - static_cast<i64>(cfs_min_vruntime);
        avg_acc -= static_cast<i64>(w) * normalized;
        if (avg_w_acc >= w) avg_w_acc -= w; else avg_w_acc = 0U;
    }

    /// @brief Promote @c min_vruntime on every tick / dequeue to prevent the
    ///        floor from dropping as entities age out.
    /// @param[in]     cfs_min_vruntime  Current min_vruntime (mutable by caller).
    /// @param[in,out] new_floor         Candidate new floor (in-out).
    /// @return The next @c min_vruntime value to assign.
    [[nodiscard]] auto update_min_vruntime(u64 cfs_min_vruntime, u64 new_floor) noexcept -> u64 {
        if (new_floor > cfs_min_vruntime) return new_floor;
        return cfs_min_vruntime + 1ULL;
    }

    /// @brief Predicate returning whether @c se is eligible to run on this
    ///        cfs_rq (i.e. its lag <= 0).
    /// @param[in] se            Entity being tested.
    /// @param[in] avg_vruntime  Current avg vruntime of the cfs_rq (see
    ///                          @c avg_vruntime() above).
    /// @return True when se is eligible (vruntime <= avg_vruntime).
    [[nodiscard]] auto entity_eligible(const sched_entity& se, u64 avg_vruntime) noexcept -> bool {
        return static_cast<i64>(se.vruntime) - static_cast<i64>(avg_vruntime) <= 0LL;
    }

    /// @brief Compute the per-entity slice in vruntime units, calibrated
    ///        to the number of queued entities (Linux's @c sched_slice).
    /// @param[in] se         Entity being scheduled.
    /// @param[in] nr_running Current cfs_rq::nr_running.
    /// @return The slice duration in vruntime units.
    [[nodiscard]] auto sched_slice(const sched_entity& se, u32 nr_running) noexcept -> u64 {
        const u32 n = (nr_running == 0U) ? 1U : nr_running;
        const u64 raw = EEVDF_DEFAULT_SLICE_NS / n;
        return calc_delta_fair(raw, se);
    }

    /// @brief Place an entity into the cfs_rq, computing its initial vruntime.
    /// @param[in,out] se              Entity to place.
    /// @param[in]     avg_vruntime    Current avg vruntime over the cfs_rq.
    /// @param[in]     min_vruntime    Current cfs_rq::min_vruntime.
    /// @param[in]     nr_running      Number of entities that will be queued
    ///                                on this cfs_rq *after* this placement
    /// @param[in]     initial         1 on initial enqueue (zero start); 0 on wakeup.
    auto place_entity(sched_entity& se, u64 avg_vruntime, u64 min_vruntime,
                      u32 nr_running, bool initial) noexcept -> void {
        const u64 initial_offset_ns = EEVDF_DEFAULT_SLICE_NS >> 1U;
        if (initial) {
            const u64 base = (avg_vruntime > min_vruntime) ? avg_vruntime : min_vruntime;
            se.vruntime = (base > initial_offset_ns) ? (base - initial_offset_ns) : 0ULL;
            se.lag = static_cast<i64>(se.vruntime) - static_cast<i64>(base);
        } else {
            se.vruntime = (se.vruntime < min_vruntime) ? min_vruntime : se.vruntime;
            se.lag = static_cast<i64>(se.vruntime) - static_cast<i64>(avg_vruntime);
        }
        se.slice = sched_slice(se, nr_running);
        se.deadline = se.vruntime + se.slice;
    }

    /// @brief Debit the running entity by the elapsed wall-time delta.
    /// @param[in,out] se        Entity receiving the debit.
    /// @param[in,out] min_vr    Mutable cfs_rq::min_vruntime reference (advanced here).
    /// @param[in]     delta     Elapsed wall-time delta (vtime units).
    auto update_curr(sched_entity& se, u64& min_vr, u64 delta) noexcept -> void {
        if (delta == 0U) return;
        se.vruntime += calc_delta_fair(delta, se);
        if (se.vruntime > min_vr) {
            min_vr = update_min_vruntime(min_vr, se.vruntime);
        }
        // Recompute eligibility when the entity crosses its deadline.
        if (se.vruntime >= se.deadline) {
            se.deadline = se.vruntime + se.slice;
            se.lag = static_cast<i64>(se.vruntime) - static_cast<i64>(min_vr);
        }
    }

    /// @brief Recompute an entity's lag after a balancer migration.
    /// @param[in,out] se          Entity being migrated.
    /// @param[in]     src_min_vr  Source cfs_rq::min_vruntime.
    /// @param[in]     dst_min_vr  Destination cfs_rq::min_vruntime.
    auto migrate_entity(sched_entity& se, u64 src_min_vr, u64 dst_min_vr) noexcept -> void {
        const u64 delta = (se.vruntime > src_min_vr) ? (se.vruntime - src_min_vr) : 0ULL;
        se.vruntime = dst_min_vr + delta;
        se.deadline = se.vruntime + se.slice;
        se.lag = static_cast<i64>(se.vruntime) - static_cast<i64>(dst_min_vr);
        se.rel_count += 1U;
    }

    /// @brief Yield: collapse @c se->vruntime to @c min_vruntime so the runner
    ///        ages backward into the queue.
    auto yield_entity(sched_entity& se, u64 min_vr) noexcept -> void {
        const u64 new_vr = (se.vruntime > min_vr) ? (se.vruntime - se.slice) : min_vr;
        se.vruntime = new_vr;
        se.deadline = se.vruntime + se.slice;
        se.lag = static_cast<i64>(se.vruntime) - static_cast<i64>(min_vr);
    }

    /// @brief Directed yield: move @c yielding_se ahead of @c target_se.
    auto yield_to_entity(sched_entity& yielding_se,
                         const sched_entity& target_se,
                         u64 min_vr) noexcept -> void {
        if (yielding_se.vruntime < target_se.vruntime) {
            const u64 delta = target_se.vruntime - yielding_se.vruntime;
            yielding_se.vruntime = (yielding_se.vruntime + delta + 1ULL) & ~0ULL;
            if (yielding_se.vruntime < min_vr) yielding_se.vruntime = min_vr;
        } else {
            yielding_se.vruntime += 1ULL;
        }
        yielding_se.deadline = yielding_se.vruntime + yielding_se.slice;
        yielding_se.lag = static_cast<i64>(yielding_se.vruntime) - static_cast<i64>(min_vr);
    }

    const classed::sched_class the_class{
        vtable_enqueue, dequeue, pick_next, tick, preempt
    };

    /// @brief Enqueue a strand into the CFS fair scheduling tree.
    /// @param[in,out] rq Run queue (caller holds @c rq.lock).
    /// @param[in,out] s  Strand being enqueued.
    /// @return Success, or @c kernel_error when SMT isolation-sensitive
    ///         placement is violated on the target CPU.
    [[nodiscard]] auto enqueue(run_queue& rq, strand& s) noexcept -> std::expected<void, lib::kernel_error> {
        if (!s.sched_policy.allow_smt_sibling
            && zxfoundation::sched::smt::isolation_violation(s, rq.cpu_id)) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        const bool initial = (s.se.deadline == 0U);

        const u64 avg = avg_vruntime(rq.min_vruntime, rq.avg_vruntime_acc, rq.avg_weight_acc);
        place_entity(s.se, avg, rq.min_vruntime, rq.nr_running + 1U, initial);
        avg_vruntime_add(s.se, rq.min_vruntime, rq.avg_vruntime_acc, rq.avg_weight_acc);

        lib::rb_node** link = &rq.fair_tree.root.node;
        lib::rb_node*  parent = nullptr;
        bool leftmost = true;

        while (*link) {
            parent = *link;
            auto* existing = rb_node_to_strand(parent);
            if (s.se.deadline < existing->se.deadline) {
                link = &parent->left;
            } else {
                link = &parent->right;
                leftmost = false;
            }
        }
        lib::rb_link_node(&s.se.fair_node, parent, link);
        lib::rb_insert_color_cached(&s.se.fair_node, rq.fair_tree, leftmost);

        s.on_rq = true;
        ++rq.nr_running;
        return {};
    }

    auto dequeue(run_queue& rq, strand& s) noexcept -> void {
        if (!s.on_rq) return;
        avg_vruntime_sub(s.se, rq.min_vruntime, rq.avg_vruntime_acc, rq.avg_weight_acc);
        lib::rb_erase_cached(&s.se.fair_node, rq.fair_tree);
        s.on_rq = false;
        --rq.nr_running;
    }

    [[nodiscard]] auto pick_next(run_queue& rq) noexcept -> strand* {
        auto* rb_left = rq.fair_tree.leftmost;
        if (!rb_left) return nullptr;

        const u64 avg = avg_vruntime(rq.min_vruntime, rq.avg_vruntime_acc, rq.avg_weight_acc);

        auto* node = rb_left;
        while (node) {
            auto* s = rb_node_to_strand(node);
            if (entity_eligible(s->se, avg)) {
                return s;
            }
            node = lib::rb_next(node);
        }

        return rb_node_to_strand(rb_left);
    }

    auto tick(run_queue& rq, strand& s) noexcept -> void {
        const u64 prev_deadline = s.se.deadline;
        update_curr(s.se, rq.min_vruntime, EEVDF_DEFAULT_SLICE_NS);

        if (s.se.deadline != prev_deadline) {
            auto* owner = static_cast<dgp::domain::domain*>(s.owner);
            owner->flags |= dgp::domain::domain_flags::need_resched;
        }
    }

    auto preempt(run_queue& rq, strand& waking) noexcept -> void {
        if (!rq.curr) return;
        if (waking.se.deadline < rq.curr->se.deadline) {
            auto* owner = static_cast<dgp::domain::domain*>(rq.curr->owner);
            owner->flags |= dgp::domain::domain_flags::need_resched;
        }
    }

    /// @brief Legacy vtable shim
    auto vtable_enqueue(run_queue& rq, strand& s) noexcept -> void {
        discard_value enqueue(rq, s);
    }

} // namespace zxfoundation::sched::fair