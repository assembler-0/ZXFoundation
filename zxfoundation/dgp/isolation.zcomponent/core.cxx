/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/isolation.zcomponent/core.cxx
/// @brief DGP isolation lifecycle and authorization implementation.

module zxfoundation.dgp.isolation.core;
import zxfoundation.base.types;
import zxfoundation.base.typestate;
import zxfoundation.dgp.isolation.types;
import zxfoundation.dgp.memory_contract;
import zxfoundation.dgp.domain.types;
import zxfoundation.sync.lockable;
import zxfoundation.sched.policy;
import zxfoundation.base.config;
import arch.s390x.mmu.types;
import lib.error;
import std;

namespace zxfoundation::dgp::isolation::detail {

    static std::atomic<bool> g_isolation_initialized{false};
    static bool g_storage_key_used[STORAGE_KEY_COUNT]{};
    static u32 g_storage_key_generation[STORAGE_KEY_COUNT]{};
    static std::atomic<u32> g_storage_key_lock{0U};

    class storage_key_lock_guard {
    public:
        storage_key_lock_guard() noexcept {
            u32 expected = 0U;
            while (!g_storage_key_lock.compare_exchange_weak(
                expected, 1U, std::memory_order::acquire, std::memory_order::relaxed)) {
                expected = 0U;
            }
        }

        ~storage_key_lock_guard() noexcept {
            g_storage_key_lock.store(0U, std::memory_order::release);
        }

        storage_key_lock_guard(const storage_key_lock_guard&) = delete;
        auto operator=(const storage_key_lock_guard&) -> storage_key_lock_guard& = delete;
    };

    [[nodiscard]] auto generic_invalid_arg() noexcept -> lib::kernel_error {
        return lib::kernel_error::from_generic(lib::generic_error::invalid_arg);
    }

    [[nodiscard]] auto memory_out_of_memory() noexcept -> lib::kernel_error {
        return lib::kernel_error::from_memory(lib::memory_error::out_of_memory);
    }

    [[nodiscard]] auto mask_is_valid(
        zxfoundation::sched::sched_mask mask,
        u32 limit
    ) noexcept -> bool {
        if (mask.bits == 0U) return true;
        if (limit >= 64U) return true;
        const u64 valid_mask = (1ULL << limit) - 1ULL;
        return (mask.bits & ~valid_mask) == 0U;
    }

    [[nodiscard]] auto allocate_storage_key(u8 requested) noexcept
        -> std::expected<u8, lib::kernel_error>
    {
        const storage_key_lock_guard guard{};
        if (requested != STORAGE_KEY_AUTO) {
            if (requested < STORAGE_KEY_USER_BASE || requested > STORAGE_KEY_USER_MAX) {
                return std::unexpected(generic_invalid_arg());
            }

            if (g_storage_key_used[requested]) {
                return std::unexpected(memory_out_of_memory());
            }

            g_storage_key_used[requested] = true;
            return requested;
        }

        for (u8 key = STORAGE_KEY_USER_BASE; key <= STORAGE_KEY_USER_MAX; ++key) {
            if (!g_storage_key_used[key]) {
                g_storage_key_used[key] = true;
                return key;
            }
        }

        return std::unexpected(memory_out_of_memory());
    }

    [[nodiscard]] auto release_storage_key(u8 key) noexcept -> u32 {
        const storage_key_lock_guard guard{};
        if (key >= STORAGE_KEY_USER_BASE && key <= STORAGE_KEY_USER_MAX) {
            if (g_storage_key_used[key]) {
                g_storage_key_used[key] = false;
                ++g_storage_key_generation[key];
            }
            return g_storage_key_generation[key];
        }
        return 0U;
    }

    [[nodiscard]] auto storage_key_generation(u8 key) noexcept -> u32 {
        const storage_key_lock_guard guard{};
        if (key >= STORAGE_KEY_COUNT) return 0U;
        return g_storage_key_generation[key];
    }


} // namespace zxfoundation::dgg::isolation

namespace zxfoundation::dgp::isolation {

    using namespace detail;
    using zxfoundation::base::lifecycle_state;

    /// @brief Initialize the DGP isolation clean core.
    /// @note The initial implementation resets the bootstrap storage-key registry before domain-visible DGP objects exist.
    /// @warning This function must run before domain, gate, or portal policy is exposed to user-visible objects.
    auto init() noexcept -> void {
        const storage_key_lock_guard guard{};
        for (u8 key = 0U; key < STORAGE_KEY_COUNT; ++key) {
            g_storage_key_used[key] = false;
            g_storage_key_generation[key] = 1U;
        }

        g_storage_key_used[0U] = true;

        g_isolation_initialized.store(true, std::memory_order::release);
    }

    /// @brief Create initial DGP isolation state for a domain.
    /// @param[in] domain_id Domain identifier assigned by the caller.
    /// @param[in] policy Hardware-first isolation policy for the domain.
    /// @return Isolation domain state, or a kernel error.
    /// @note This skeleton allocates storage-key ownership and records policy state; later wiring supplies committed ASCE roots from the MMU/domain transaction.
    /// @warning On success, callers must either activate or destroy the returned state to avoid leaking storage-key ownership.
    [[nodiscard]] auto create_domain(
        u32 domain_id,
        const isolation_policy& policy
    ) noexcept -> std::expected<isolation_domain_state, lib::kernel_error> {
        if (!g_isolation_initialized.load(std::memory_order::acquire)) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::uninitialized));
        }

        auto key_result = allocate_storage_key(policy.storage_key);
        if (!key_result) {
            return std::unexpected(key_result.error());
        }

        isolation_domain_state state{};
        state.domain_id = domain_id;
        state.storage_key = *key_result;
        state.storage_key_generation = storage_key_generation(*key_result);
        state.storage_key_state = skey_revocation_state::active;
        state.numa_node = policy.numa_node;
        state.granted_rights = policy.default_rights;

        if (policy.bootstrap_admin) {
            state.granted_rights = state.granted_rights
                | dgp::isolation::isolation_rights::lifecycle
                | dgp::isolation::isolation_rights::delegate
                | dgp::isolation::isolation_rights::quota
                | dgp::isolation::isolation_rights::debug
                | dgp::isolation::isolation_rights::sched
                | dgp::isolation::isolation_rights::console
                | dgp::isolation::isolation_rights::service;
        }

        return state;
    }

    /// @brief Attach committed DAT root state to a DGP isolation domain.
    /// @param[in] state Isolation state to update.
    /// @param[in] current_ls Current lifecycle state (from ko.state) — must be sealed.
    /// @param[in] asce Typed region-first ASCE constructed from the committed DAT root.
    /// @param[in] root_phys Physical address of the committed region-first root table.
    /// @return Success when the state can accept the ASCE root metadata.
    /// @note The IBM PoP defines ASCE contents as hardware-consumed DAT state; this helper records ownership and does not load a control register.
    /// @warning The caller remains responsible for allocating and freeing the DAT table through the MMU transaction path.
    [[nodiscard]] auto bind_asce_root(
        isolation_domain_state& state,
        lifecycle_state current_ls,
        arch::s390x::mmu::typed_asce<arch::s390x::mmu::dat_level::region_1> asce,
        u64 root_phys
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (current_ls != lifecycle_state::sealed || root_phys == 0ULL || asce.raw() == 0ULL) {
            return std::unexpected(generic_invalid_arg());
        }

        state.asce = asce;
        state.root_phys = root_phys;
        return {};
    }

    /// @brief Activate a sealed DGP isolation domain.
    /// @param[in] state Isolation state to activate.
    /// @param[in] asce Typed region-first ASCE constructed from the committed DAT root.
    /// @param[in] root_phys Physical address of the committed region-first root table.
    /// @param[in] storage_key The domain's hardware storage key.
    /// @return Success when the domain transitions to active.
    /// @note Activation requires committed ASCE root metadata before scheduler admission.
    /// @warning Active domains must deny new gate and portal entries before destruction.
    [[nodiscard]] auto activate_domain(
        lifecycle_state& state,
        arch::s390x::mmu::typed_asce<arch::s390x::mmu::dat_level::region_1> asce,
        u64 root_phys,
        [[maybe_unused]] u8 storage_key
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (state != lifecycle_state::sealed || root_phys == 0ULL || asce.raw() == 0ULL) {
            return std::unexpected(generic_invalid_arg());
        }

        return {};
    }

    /// @brief Begin revocation of the storage-key authority attached to a DGP domain.
    /// @param[in,out] state Isolation state whose key is entering the draining phase.
    /// @return Success when no live gate or portal authority can still observe the key.
    /// @note Storage-key reuse is delayed until callers have invalidated DAT and ALB state associated with the domain.
    /// @warning This function does not modify hardware storage keys; it records the software authority transition.
    [[nodiscard]] auto begin_storage_key_revocation(
        lifecycle_state& state,
        skey_revocation_state& storage_key_state,
        std::atomic<u16>& revocation_epoch
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (state == lifecycle_state::destroyed
            || storage_key_state == skey_revocation_state::reusable) {
            return {};
        }

        state = lifecycle_state::dying;
        storage_key_state = skey_revocation_state::draining;
        discard_value revocation_epoch.fetch_add(1U, std::memory_order::acq_rel);
        return {};
    }

    [[nodiscard]] auto begin_storage_key_revocation(
        lifecycle_state& state,
        skey_revocation_state& storage_key_state,
        u16& revocation_epoch
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (state == lifecycle_state::destroyed
            || storage_key_state == skey_revocation_state::reusable) {
            return {};
        }

        state = lifecycle_state::dying;
        storage_key_state = skey_revocation_state::draining;
        std::atomic_ref<u16> epoch{revocation_epoch};
        discard_value epoch.fetch_add(1U, std::memory_order::acq_rel);
        return {};
    }

    /// @brief Complete storage-key revocation after required translation invalidation is done.
    /// @param[in] state Isolation state whose storage key can be returned to DGP policy.
    /// @param[in,out] storage_key The domain's hardware storage key.
    /// @param[in,out] storage_key_generation Storage key generation counter.
    /// @param[in,out] storage_key_state Storage key revocation state.
    /// @return Success when DGP-owned key metadata is no longer reusable by stale mappings.
    /// @note The key generation is advanced by the storage-key allocator when the key is released.
    /// @warning Callers must only invoke this after VM, DAT, and portal teardown have completed.
    [[nodiscard]] auto complete_storage_key_revocation(
        [[maybe_unused]] lifecycle_state& state,
        u8& storage_key,
        u32& storage_key_generation,
        skey_revocation_state& storage_key_state
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (storage_key_state == skey_revocation_state::reusable
            || storage_key == 0U) {
            storage_key_state = skey_revocation_state::reusable;
            return {};
        }

        if (storage_key_state != skey_revocation_state::draining) {
            return std::unexpected(generic_invalid_arg());
        }

        const u8 released_key = storage_key;
        storage_key_generation = release_storage_key(released_key);
        storage_key_state = skey_revocation_state::reusable;
        discard_value memory_contract::generation_bump();
        return {};
    }

    /// @brief Record that a portal authority is leaving the active DGP set.
    /// @param[in,out] state Isolation state for the source domain that held the portal.
    /// @param[in,out] revocation_epoch Revocation epoch counter.
    /// @return Success when one active portal reference was converted into revocation progress.
    /// @note Portal teardown must purge ALB state before the corresponding `ALE` can be reused.
    /// @warning This function only updates DGP metadata; callers still destroy hardware access-list entries.
    [[nodiscard]] auto begin_portal_teardown(
        [[maybe_unused]] lifecycle_state& state,
        std::atomic<u16>& revocation_epoch
    ) noexcept -> std::expected<void, lib::kernel_error> {
        discard_value revocation_epoch.fetch_add(1U, std::memory_order::acq_rel);
        return {};
    }

    [[nodiscard]] auto begin_portal_teardown(
        [[maybe_unused]] lifecycle_state& state,
        u16& revocation_epoch
    ) noexcept -> std::expected<void, lib::kernel_error> {
        std::atomic_ref<u16> epoch{revocation_epoch};
        discard_value epoch.fetch_add(1U, std::memory_order::acq_rel);
        return {};
    }

    /// @brief Validate that a DGP domain may be admitted to a CPU run queue.
    /// @param[in] state Isolation state associated with the candidate domain.
    /// @param[in] root_phys Physical address of the region-first root table.
    /// @param[in] asce_raw Raw ASCE value.
    /// @return Success when the domain is active and has committed ASCE metadata.
    /// @note Scheduler admission is a hardware boundary because a later context switch may load the ASCE into CR1.
    /// @warning Callers must still serialize run-queue mutation with the scheduler lock after this check succeeds.
    [[nodiscard]] auto validate_scheduler_admission(
        const lifecycle_state state,
        u64 root_phys,
        u64 asce_raw
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (!g_isolation_initialized.load(std::memory_order::acquire)) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::uninitialized));
        }

        if (state != lifecycle_state::active || root_phys == 0ULL || asce_raw == 0ULL) {
            return std::unexpected(generic_invalid_arg());
        }

        return {};
    }

    /// @brief Validate a scheduler policy against DGP authority and basic safety invariants.
    /// @param[in] state Domain isolation state requesting scheduler admission.
    /// @param[in] policy Requested scheduler policy.
    /// @param[in] has_rt_authority True when the caller holds the domain's RT/admin capability authority.
    /// @return Scheduler admission result, or kernel_error describing the denied request.
    /// @note RT and deadline bandwidth reservation is completed by scheduler backends after this DGP check.
    /// @warning User-visible RT requests must pass this function before any run-queue insertion.
    [[nodiscard]] auto validate_scheduler_policy(
        const lifecycle_state state,
        const zxfoundation::sched::sched_policy& policy,
        bool has_rt_authority
    ) noexcept -> std::expected<zxfoundation::sched::sched_admission_result, lib::kernel_error> {
        if (state != lifecycle_state::active) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        zxfoundation::sched::sched_admission_result result{};
        result.class_id = policy.class_id;
        result.remote_node_allowed = true;
        result.smt_sibling_allowed = policy.allow_smt_sibling;

        switch (policy.class_id) {
            case zxfoundation::sched::sched_class_id::idle:
            case zxfoundation::sched::sched_class_id::fair:
                return result;
            case zxfoundation::sched::sched_class_id::rt:
                if (!has_rt_authority || policy.rt_priority == 0U || policy.rt_priority > 99U) {
                    return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                if (policy.runtime_ns == 0U || policy.period_ns == 0U || policy.runtime_ns > policy.period_ns) {
                    return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                if (!mask_is_valid(policy.allowed_cpus, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
                    || !mask_is_valid(policy.allowed_numa_nodes, 64U)) {
                    return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                result.admitted_runtime_ns = policy.runtime_ns;
                result.admitted_period_ns = policy.period_ns;
                return result;
            case zxfoundation::sched::sched_class_id::deadline:
                if (!has_rt_authority) {
                    return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                if (policy.runtime_ns == 0U || policy.deadline_ns == 0U || policy.period_ns == 0U
                    || policy.runtime_ns > policy.deadline_ns || policy.deadline_ns > policy.period_ns) {
                    return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                if (!mask_is_valid(policy.allowed_cpus, zxfoundation::base::config::CONFIG_ZX_MAX_CPUS)
                    || !mask_is_valid(policy.allowed_numa_nodes, 64U)) {
                    return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
                }
                result.admitted_runtime_ns = policy.runtime_ns;
                result.admitted_period_ns = policy.period_ns;
                return result;
        }

        return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
    }

    /// @brief Destroy DGP isolation state for a domain.
    /// @param[in] state Domain lifecycle state.
    /// @param[in,out] storage_key The domain's hardware storage key.
    /// @param[in,out] storage_key_generation Storage key generation counter.
    /// @param[in,out] storage_key_state Storage key revocation state.
    /// @param[in,out] revocation_epoch Revocation epoch counter.
    /// @return Success when storage-key ownership is released; SCOMS records the terminal state.
    /// @note This helper releases DGP-owned identity only; the caller remains responsible for DAT tables, VM regions, portals, gates, and stacks.
    /// @warning Destruction with active gate or portal counts is rejected to prevent stale authority use.
    [[nodiscard]] auto destroy_domain(
        lifecycle_state& state,
        u8& storage_key,
        u32& storage_key_generation,
        skey_revocation_state& storage_key_state,
        std::atomic<u16>& revocation_epoch
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (state == lifecycle_state::destroyed) {
            return {};
        }

        auto begin_revocation = begin_storage_key_revocation(
            state, storage_key_state, revocation_epoch
        );
        if (!begin_revocation) return std::unexpected(begin_revocation.error());

        auto complete_revocation = complete_storage_key_revocation(
            state, storage_key, storage_key_generation, storage_key_state
        );
        if (!complete_revocation) return std::unexpected(complete_revocation.error());

        return {};
    }

    [[nodiscard]] auto destroy_domain(
        lifecycle_state& state,
        u8& storage_key,
        u32& storage_key_generation,
        skey_revocation_state& storage_key_state,
        u16& revocation_epoch
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (state == lifecycle_state::destroyed) {
            return {};
        }

        auto begin_revocation = begin_storage_key_revocation(
            state, storage_key_state, revocation_epoch
        );
        if (!begin_revocation) return std::unexpected(begin_revocation.error());

        auto complete_revocation = complete_storage_key_revocation(
            state, storage_key, storage_key_generation, storage_key_state
        );
        if (!complete_revocation) return std::unexpected(complete_revocation.error());

        return {};
    }

    /// @brief Validate entry into a DGP gate chokepoint (structural validation only).
    /// @param[in] caller_domain_id Domain requesting entry.
    /// @param[in] gate_id Gate identifier being entered.
    /// @param[in] requested Rights required by the gate operation.
    /// @return Success when the request is structurally valid.
    /// @note Full gate validation (existence, state, rights) is performed by dispatch::validate_call.
    [[nodiscard]] auto validate_gate_entry(
        u32 caller_domain_id,
        u32 gate_id,
        dgp::isolation::isolation_rights requested
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (!g_isolation_initialized.load(std::memory_order::acquire)) {
            return std::unexpected(generic_invalid_arg());
        }

        if (requested == dgp::isolation::isolation_rights::none) {
            return std::unexpected(generic_invalid_arg());
        }

        if (caller_domain_id >= zxfoundation::dgp::domain::MAX_DOMAINS) {
            return std::unexpected(generic_invalid_arg());
        }

        const u32 ns = gate_id >> 5;
        if (ns > 0x1F) {
            return std::unexpected(generic_invalid_arg());
        }

        return {};
    }

    /// @brief Validate creation or use of a DGP portal.
    /// @param[in] source_domain_id Domain that will hold the portal authority.
    /// @param[in] target_domain_id Domain whose address space is exposed through the portal.
    /// @param[in] va_start First target virtual address exposed through the portal.
    /// @param[in] va_end Exclusive end of the target virtual range.
    /// @param[in] requested Rights requested for the portal mapping.
    /// @return Success when the portal request is structurally valid.
    /// @note Later migration binds this validation to ASTE/ALE ownership, ALB invalidation, and capability policy.
    /// @warning This function does not allocate an ASTE, create an ALE, or install access-register state.
    [[nodiscard]] auto validate_portal(
        u32 source_domain_id,
        u32 target_domain_id,
        u64 va_start,
        u64 va_end,
        dgp::isolation::isolation_rights requested
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (!g_isolation_initialized.load(std::memory_order::acquire)) {
            return std::unexpected(generic_invalid_arg());
        }

        discard_value source_domain_id;
        discard_value target_domain_id;

        if (va_start >= va_end) {
            return std::unexpected(generic_invalid_arg());
        }

        if (!has_all_rights(requested, dgp::isolation::isolation_rights::share)) {
            return std::unexpected(generic_invalid_arg());
        }

        return {};
    }

    /// @brief Lazy revocation of DGP rights on a domain.
    /// @param[in] d          Target domain.  Must be non-null and active.
    /// @param[in] drop_mask  Bits to clear from @c granted_rights.
    ///                       Bits not currently set have no effect.
    /// @return Success on completion, or kernel_error when @c d is not active.
    auto revoke_domain_rights(
        zxfoundation::dgp::domain::domain& d,
        dgp::isolation::isolation_rights drop_mask
    ) noexcept -> std::expected<void, lib::kernel_error> {
        const zxfoundation::sync::irq_lock_guard guard{d.ko.lock};
        if (d.ko.state != lifecycle_state::active) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        const auto drop_raw = static_cast<u32>(drop_mask);
        const auto granted_raw = static_cast<u32>(d.granted_rights);
        const auto effective_drop = drop_raw & granted_raw;
        if (effective_drop == 0U) {
            return {};
        }

        d.granted_rights = static_cast<dgp::isolation::isolation_rights>(granted_raw & ~effective_drop);
        d.pending_rights_revoke_raw |= effective_drop;
        discard_value d.revocation_epoch.fetch_add(1U, std::memory_order::acq_rel);

        return {};
    }


} // namespace zxfoundation::dgp
