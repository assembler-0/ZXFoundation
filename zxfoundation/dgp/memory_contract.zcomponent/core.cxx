/// SPDX-License-Identifier: Apache-2.0
/// @file zxfoundation/dgp/memory_contract.zcomponent/core.cxx
/// @brief DGP memory-contract adapter validation implementation.

module zxfoundation.dgp.memory_contract.core;
import zxfoundation.base.types;
import zxfoundation.dgp.isolation.types;
import arch.s390x.mmu.types;
import arch.s390x.mmu.consts;
import lib.error;
import std;

namespace zxfoundation::dgp::memory_contract::detail {

    /// @brief Live monotonic generation.
    no_export std::atomic<u64> g_generation{1ULL};

} // namespace zxfoundation::dgp (implementation state)

namespace zxfoundation::dgp::memory_contract {

    using detail::g_generation;

    /// @brief Advance the global memory-contract generation.
    auto generation_bump() noexcept -> u64 {
        return g_generation.fetch_add(1ULL, std::memory_order::acq_rel) + 1ULL;
    }
    /// @brief Validate a virtual address range before a VM or portal operation.
    /// @param[in] va_start First virtual address in the range.
    /// @param[in] va_end Exclusive end of the virtual range.
    /// @return Success when the range is ordered, non-empty, and page-aligned.
    /// @note The contract checks generic range safety only; ownership and DAT effects are checked by later adapters.
    /// @warning Passing this check does not install or invalidate any DAT entry.
    [[nodiscard]] auto validate_range(
        u64 va_start,
        u64 va_end
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (va_end == 0ULL || va_start >= va_end) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        if (!is_page_aligned(va_start) || !is_page_aligned(va_end)) {
            return std::unexpected(lib::kernel_error::from_memory(lib::memory_error::alignment_error));
        }

        return {};
    }
    /// @brief Validate a storage key for a DGP memory request.
    /// @param[in] storage_key Four-bit access-control key value.
    /// @return Success when the key is architecturally representable.
    /// @note The IBM PoP defines the storage-key access-control field as four bits.
    /// @warning This check does not allocate, reserve, or release a key.
    [[nodiscard]] auto validate_storage_key(
        u8 storage_key
    ) noexcept -> std::expected<void, lib::kernel_error> {
        if (storage_key == isolation::STORAGE_KEY_AUTO) {
            return {};
        }

        if (storage_key >= isolation::STORAGE_KEY_COUNT) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::out_of_range));
        }

        return {};
    }
    /// @brief Validate a complete memory-contract request.
    /// @param[in] request DGP memory request to validate.
    /// @return Success when the generic range, length, and storage-key constraints are satisfied.
    /// @note Later migration waves will bind this request to VM regions, DAT permissions, NUMA zones, and TLB scope.
    /// @warning This adapter performs no allocation and installs no mapping.
    [[nodiscard]] auto validate_request(
        const contract_request& request
    ) noexcept -> std::expected<void, lib::kernel_error> {
        auto range_result = validate_range(request.va_start, request.va_end);
        if (!range_result) {
            return std::unexpected(range_result.error());
        }

        if (request.length == 0ULL || request.length != (request.va_end - request.va_start)) {
            return std::unexpected(lib::kernel_error::from_generic(lib::generic_error::invalid_arg));
        }

        if ((request.prot.raw() & arch::s390x::mmu::PTE_NOEXEC) == 0ULL
            && (request.prot.raw() & arch::s390x::mmu::PTE_PROTECT) == 0ULL) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }

        if (request.phys_start != 0ULL && !is_page_aligned(request.phys_start)) {
            return std::unexpected(lib::kernel_error::from_memory(lib::memory_error::alignment_error));
        }

        return validate_storage_key(request.storage_key);
    }
    /// @brief Return the current DGP memory-policy generation.
    [[nodiscard]] auto contract_generation() noexcept -> u64 {
        return g_generation.load(std::memory_order::acquire);
    }
    /// @brief Resolve a storage key value for region metadata.
    /// @param[in] storage_key Requested storage key, or `isolation::STORAGE_KEY_AUTO`.
    /// @return Concrete architected storage-key access-control value.
    [[nodiscard]] auto resolve_storage_key(
        u8 storage_key
    ) noexcept -> std::expected<u8, lib::kernel_error> {
        auto key_result = validate_storage_key(storage_key);
        if (!key_result) return std::unexpected(key_result.error());

        if (storage_key == isolation::STORAGE_KEY_AUTO) {
            return 0U;
        }

        return storage_key;
    }
    /// @brief Authorize creation of a VM mapping authority record.
    /// @param[in] request Complete VM/DAT/storage-key/NUMA request.
    /// @return DGP policy metadata to store in the owning `vm_region`.
    /// @note The result records conservative invalidation needs; actual `PTLB`/`IDTE`/`IPTE` dispatch is wired in the DAT invalidation wave.
    /// @warning This function authorizes policy only; it does not allocate frames or install DAT entries.
    [[nodiscard]] auto authorize_mapping(
        const contract_request& request
    ) noexcept -> std::expected<contract_result, lib::kernel_error> {
        auto validate_result = validate_request(request);
        if (!validate_result) return std::unexpected(validate_result.error());

        if (request.scope == memory_scope::portal_shared
            && !memory_access_has(request.access, memory_access::share)) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }

        if ((request.prot.raw() & arch::s390x::mmu::PTE_NOEXEC) == 0ULL
            && (request.prot.raw() & arch::s390x::mmu::PTE_PROTECT) == 0ULL) {
            return std::unexpected(lib::kernel_error::from_memory(
                lib::memory_error::capability_invalid));
        }

        auto key = resolve_storage_key(request.storage_key);
        if (!key) return std::unexpected(key.error());

        return contract_result{
            .storage_key = *key,
            .numa_node = request.numa_node,
            .invalidate = request.phys_start != 0ULL
                ? invalidation_scope::address_space
                : invalidation_scope::none,
            .required_rights = required_rights(request.access, request.scope),
            .generation = contract_generation(),
            .access = request.access,
            .scope = request.scope,
        };
    }
    /// @brief Authorize a VM protection change.
    /// @param[in] request Complete VM/DAT/storage-key/NUMA request for the new protection.
    /// @param[in] old_prot Previous hardware protection bits.
    /// @return DGP policy metadata to store in affected VM regions.
    /// @note Any changed DAT protection is conservatively marked address-space scoped until CPU-ASCE attachment tracking is complete.
    /// @warning The caller must perform DAT invalidation before reusing frames or relaxing storage-key authority.
    [[nodiscard]] auto authorize_protect(
        const contract_request& request,
        arch::s390x::mmu::page_prot old_prot
    ) noexcept -> std::expected<contract_result, lib::kernel_error> {
        auto result = authorize_mapping(request);
        if (!result) return std::unexpected(result.error());

        result->invalidate = (old_prot.raw() == request.prot.raw())
            ? invalidation_scope::none
            : invalidation_scope::address_space;
        return *result;
    }
    /// @brief Authorize removal of a VM mapping authority record.
    /// @param[in] request Complete VM/DAT/storage-key/NUMA request describing the removed range.
    /// @return DGP policy metadata describing the required teardown authority.
    /// @note Physical frame, portal, storage-key, and DAT reuse must wait for the returned invalidation scope.
    /// @warning This function does not perform teardown; it only selects policy metadata.
    [[nodiscard]] auto authorize_unmap(
        const contract_request& request
    ) noexcept -> std::expected<contract_result, lib::kernel_error> {
        auto result = authorize_mapping(request);
        if (!result) return std::unexpected(result.error());

        result->invalidate = memory_access_has(request.access, memory_access::share)
            ? invalidation_scope::alb_global
            : invalidation_scope::address_space;
        return *result;
    }

} // namespace zxfoundation::dgp
